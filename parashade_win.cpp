// parashade_win.cpp — Parashade v0.3 (Windows-focused)
// MSVC:   cl /std:c++17 /O2 /EHsc /Fe:parashade.exe parashade_win.cpp
// Clang:  clang-cl /std:c++17 /O2 /EHsc /Fe:parashade.exe parashade_win.cpp
// Usage:  type file.psd | parashade.exe --run
//         type file.psd | parashade.exe --emit
//         type file.psd | parashade.exe --emit-nasm .out
//
// New in v0.3
// - Conditionals (if/else) via JZ/JMP, label patching
// - Comparison ops: gt/lt/ge/le/eq/ne (const-folding or IR ops)
// - Arrays with bounds checks: arr_new/arr_get/arr_set
//   • VM: handle-based heap
//   • NASM: Windows HeapAlloc (GetProcessHeap)
// - NASM(PE) emitter expanded to cover new ops (incl. cmp/jumps/arrays)
// - Range-checked capsules + superlatives + warnings -> .meta.json
//
// Notes: Minimal subset for education; extend as needed.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using std::string;

// ----------------- Utils
static inline string trim(string s){
    size_t a=0; while(a<s.size() && isspace((unsigned char)s[a]))++a;
    size_t b=s.size(); while(b>a && isspace((unsigned char)s[b-1]))--b;
    return s.substr(a,b-a);
}
static inline bool starts_with(const string& s, const string& p){ return s.rfind(p,0)==0; }
static inline string lowerc(string s){ for(char& c:s) c=char(tolower((unsigned char)c)); return s; }

// ----------------- Long-form → Core normalizer (brief)
static string normalize_longform(const string& in){
    static const std::vector<std::pair<std::regex,string>> M = {
        {std::regex("\\bdeclare\\s+explicit\\s+integer\\s+named\\s+"), "let int "},
        {std::regex("\\bdeclare\\s+implicit\\s+named\\s+"),            "let "},
        {std::regex("\\bequals\\b"),                                   "="},
        {std::regex("\\bend\\b"),                                      ""},
        {std::regex("\\bplus\\b"),                                     "+"},
        {std::regex("\\bmodule\\b"),                                   "module"},
        {std::regex("\\bscope\\b"),                                    "scope"},
        {std::regex("\\brange\\b"),                                    "range"},
        {std::regex("\\breturn\\b"),                                   "return"},
    };
    std::ostringstream out;
    std::istringstream iss(in); string line;
    while(std::getline(iss,line)){
        auto sc=line.find(';'); if(sc!=string::npos) line=line.substr(0,sc);
        string L=line;
        for(auto& p:M) L=std::regex_replace(L,p.first,p.second);
        out<<trim(L)<<"\n";
    }
    return out.str();
}

// ----------------- Lexer
enum class Tok {
    End, Ident, Number,
    Colon, Equals, Plus, Comma,
    LParen, RParen,
    KwModule, KwScope, KwRange, KwLet, KwInt, KwArr, KwReturn, KwEnd, KwIf, KwElse
};
struct Token{ Tok t; string s; int line; };

struct Lexer{
    std::vector<Token> toks; size_t i=0;
    explicit Lexer(const string& src){
        std::istringstream iss(src); string line; int ln=0;
        while(std::getline(iss,line)){
            ++ln;
            auto sc=line.find(';'); if(sc!=string::npos) line=line.substr(0,sc);
            std::istringstream ls(line);
            for(;;){
                int c=ls.peek(); if(c==EOF) break;
                if(isspace(c)){ ls.get(); continue; }
                if(c=='('){ toks.push_back({Tok::LParen,"(",ln}); ls.get(); continue; }
                if(c==')'){ toks.push_back({Tok::RParen,")",ln}); ls.get(); continue; }
                if(c==','){ toks.push_back({Tok::Comma,",",ln}); ls.get(); continue; }
                if(c==':'){ toks.push_back({Tok::Colon,":",ln}); ls.get(); continue; }
                if(c=='='){ toks.push_back({Tok::Equals,"=",ln}); ls.get(); continue; }
                if(c=='+'){ toks.push_back({Tok::Plus,"+",ln}); ls.get(); continue; }

                if(std::isalpha(c) || c=='_'){
                    string id; while(std::isalnum(ls.peek())||ls.peek()=='_') id.push_back(char(ls.get()));
                    string lid=lowerc(id);
                    if(lid=="module") toks.push_back({Tok::KwModule,id,ln});
                    else if(lid=="scope") toks.push_back({Tok::KwScope,id,ln});
                    else if(lid=="range") toks.push_back({Tok::KwRange,id,ln});
                    else if(lid=="let") toks.push_back({Tok::KwLet,id,ln});
                    else if(lid=="int") toks.push_back({Tok::KwInt,id,ln});
                    else if(lid=="arr") toks.push_back({Tok::KwArr,id,ln});
                    else if(lid=="return") toks.push_back({Tok::KwReturn,id,ln});
                    else if(lid=="end") toks.push_back({Tok::KwEnd,id,ln});
                    else if(lid=="if") toks.push_back({Tok::KwIf,id,ln});
                    else if(lid=="else") toks.push_back({Tok::KwElse,id,ln});
                    else toks.push_back({Tok::Ident,lid,ln});
                    continue;
                }
                // numbers
                if(c=='0'){
                    string num; num.push_back(char(ls.get()));
                    if(tolower(ls.peek())=='x'){ num.push_back(char(ls.get()));
                        while(std::isxdigit(ls.peek())||ls.peek()=='_') num.push_back(char(ls.get()));
                        toks.push_back({Tok::Number,num,ln}); continue;
                    } else {
                        while(std::isdigit(ls.peek())) num.push_back(char(ls.get()));
                        toks.push_back({Tok::Number,num,ln}); continue;
                    }
                }
                if(std::isdigit(c)){
                    string num; while(std::isdigit(ls.peek())) num.push_back(char(ls.get()));
                    toks.push_back({Tok::Number,num,ln}); continue;
                }
                ls.get(); // skip unknown
            }
        }
        toks.push_back({Tok::End,"",ln});
    }
    const Token& peek() const { return toks[i]; }
    Token pop(){ return toks[i++]; }
    bool accept(Tok t){ if(peek().t==t){ ++i; return true; } return false; }
    void expect(Tok t, const char* msg){ if(!accept(t)) throw std::runtime_error(string("Parse error: expected ")+msg+" at line "+std::to_string(peek().line)); }
};

// ----------------- AST
struct Expr{
    enum Kind{ Num, Var, Add, Call } kind;
    int line=0;
    uint64_t val=0; string name;
    std::unique_ptr<Expr> a,b;
    std::vector<std::unique_ptr<Expr>> args;
    static std::unique_ptr<Expr> num(uint64_t v,int ln){ auto p=std::make_unique<Expr>(); p->kind=Num; p->val=v; p->line=ln; return p; }
    static std::unique_ptr<Expr> var(string n,int ln){ auto p=std::make_unique<Expr>(); p->kind=Var; p->name=std::move(n); p->line=ln; return p; }
    static std::unique_ptr<Expr> add(std::unique_ptr<Expr>A,std::unique_ptr<Expr>B,int ln){ auto p=std::make_unique<Expr>(); p->kind=Add; p->a=std::move(A); p->b=std::move(B); p->line=ln; return p; }
    static std::unique_ptr<Expr> call(string n,std::vector<std::unique_ptr<Expr>> as,int ln){ auto p=std::make_unique<Expr>(); p->kind=Call; p->name=std::move(n); p->args=std::move(as); p->line=ln; return p; }
};

struct Stmt{
    enum Kind{ Let, Ret, If } kind;
    int line=0;
    // Let
    enum EType { T_Implicit, T_Int, T_Arr } etype = T_Implicit;
    string name; std::unique_ptr<Expr> expr;
    // Ret
    // If
    std::unique_ptr<Expr> cond;
    std::vector<Stmt> thenBody, elseBody;
    static Stmt makeLet(string n,EType et,std::unique_ptr<Expr>e,int ln){ Stmt s; s.kind=Let; s.name=std::move(n); s.etype=et; s.expr=std::move(e); s.line=ln; return s; }
    static Stmt makeRet(std::unique_ptr<Expr>e,int ln){ Stmt s; s.kind=Ret; s.expr=std::move(e); s.line=ln; return s; }
    static Stmt makeIf(std::unique_ptr<Expr>c,std::vector<Stmt>t,std::vector<Stmt>e,int ln){ Stmt s; s.kind=If; s.cond=std::move(c); s.thenBody=std::move(t); s.elseBody=std::move(e); s.line=ln; return s; }
};

struct Func{ string name; int line=0; std::vector<Stmt> body; };
struct Module{ string name; Func mainFn; };

// ----------------- Parser
struct Parser{
    Lexer& L; explicit Parser(Lexer& l):L(l){}
    Module parseModule(){
        L.expect(Tok::KwModule,"module");
        auto id=L.pop(); if(id.t!=Tok::Ident) throw std::runtime_error("module: expected name");
        L.expect(Tok::Colon,":");
        Module m; m.name=lowerc(id.s);
        m.mainFn=parseScope();
        return m;
    }
    Func parseScope(){
        L.expect(Tok::KwScope,"scope"); auto id=L.pop();
        if(id.t!=Tok::Ident || lowerc(id.s)!="main") throw std::runtime_error("only 'scope main' supported");
        L.expect(Tok::KwRange,"range"); auto r=L.pop(); if(r.t!=Tok::Ident) throw std::runtime_error("range: expected name");
        L.expect(Tok::Colon,":");
        Func f; f.name="main"; f.line=id.line;
        while(L.peek().t!=Tok::KwEnd && L.peek().t!=Tok::End){ f.body.push_back(parseStmt()); }
        L.expect(Tok::KwEnd,"end"); return f;
    }
    Stmt parseStmt(){
        if(L.peek().t==Tok::KwLet){
            auto letTok=L.pop(); Stmt::EType et=Stmt::T_Implicit;
            if(L.accept(Tok::KwInt)) et=Stmt::T_Int;
            else if(L.accept(Tok::KwArr)) et=Stmt::T_Arr;
            auto id=L.pop(); if(id.t!=Tok::Ident) throw std::runtime_error("let: expected name");
            L.expect(Tok::Equals,"=");
            auto e=parseExpr();
            return Stmt::makeLet(id.s,et,std::move(e),letTok.line);
        }
        if(L.peek().t==Tok::KwReturn){
            auto rt=L.pop(); auto e=parseExpr(); return Stmt::makeRet(std::move(e),rt.line);
        }
        if(L.peek().t==Tok::KwIf){
            auto it=L.pop(); L.expect(Tok::LParen,"("); auto c=parseExpr(); L.expect(Tok::RParen,")"); L.expect(Tok::Colon,":");
            std::vector<Stmt> thenB, elseB;
            while(L.peek().t!=Tok::KwElse && L.peek().t!=Tok::KwEnd && L.peek().t!=Tok::End){ thenB.push_back(parseStmt()); }
            if(L.accept(Tok::KwElse)){
                L.expect(Tok::Colon,":");
                while(L.peek().t!=Tok::KwEnd && L.peek().t!=Tok::End){ elseB.push_back(parseStmt()); }
            }
            L.expect(Tok::KwEnd,"end");
            return Stmt::makeIf(std::move(c),std::move(thenB),std::move(elseB),it.line);
        }
        throw std::runtime_error("Unknown statement at line "+std::to_string(L.peek().line));
    }
    // expr := primary ('+' primary)*
    std::unique_ptr<Expr> parseExpr(){
        auto t=parsePrimary();
        while(L.accept(Tok::Plus)){ auto r=parsePrimary(); t=Expr::add(std::move(t),std::move(r),r->line); }
        return t;
    }
    std::unique_ptr<Expr> parsePrimary(){
        auto tk=L.pop();
        if(tk.t==Tok::Number){
            uint64_t v=0;
            if(starts_with(tk.s,"0x")||starts_with(tk.s,"0X")){
                auto s=tk.s.substr(2); s.erase(std::remove(s.begin(),s.end(),'_'),s.end());
                std::stringstream ss; ss<<std::hex<<s; ss>>v;
            } else { std::stringstream ss; ss<<tk.s; ss>>v; }
            return Expr::num(v,tk.line);
        } else if(tk.t==Tok::Ident){
            if(L.accept(Tok::LParen)){
                std::vector<std::unique_ptr<Expr>> args;
                if(L.peek().t!=Tok::RParen){ args.push_back(parseExpr()); while(L.accept(Tok::Comma)) args.push_back(parseExpr()); }
                L.expect(Tok::RParen,")");
                return Expr::call(lowerc(tk.s),std::move(args),tk.line);
            }
            return Expr::var(lowerc(tk.s),tk.line);
        } else if(tk.t==Tok::LParen){
            auto e=parseExpr(); L.expect(Tok::RParen,")"); return e;
        }
        throw std::runtime_error("Expected primary at line "+std::to_string(tk.line));
    }
};

// ----------------- Types / Locals / Warnings
struct Type{ enum K{ Int, Arr } k; };
struct Local{ string name; Type ty; int index; int declLine; bool explicitDeclared=false; };

struct Typer{
    std::unordered_map<string,Local> locals;
    int nextIdx=0;
    struct Warning{ string code,msg; int line; };
    std::vector<Warning> warns;

    void declare_local(const string& n, int line, bool explicitType, Type::K k){
        if(!locals.count(n)){
            locals[n]=Local{n,Type{k},nextIdx++,line,explicitType};
            if(!explicitType){
                warns.push_back({"W001",(k==Type::Int? "implicit integer":"implicit array")+" type inferred for '"+n+"'",line});
            }
        }
    }
    bool is_const_expr(const Expr* e, uint64_t& out) const{
        switch(e->kind){
            case Expr::Num: out=e->val; return true;
            case Expr::Var: return false;
            case Expr::Add:{ uint64_t A,B; if(is_const_expr(e->a.get(),A)&&is_const_expr(e->b.get(),B)){ out=A+B; return true;} return false; }
            case Expr::Call:{
                auto nm=e->name;
                if((nm=="max"||nm=="min") && e->args.size()==2){ uint64_t A,B; if(is_const_expr(e->args[0].get(),A)&&is_const_expr(e->args[1].get(),B)){ out=(nm=="max")? (std::max<uint64_t>(A,B)):(std::min<uint64_t>(A,B)); return true; } return false; }
                if(nm=="ever_exact" && e->args.size()==1){ uint64_t X; if(is_const_expr(e->args[0].get(),X)){ out=X; return true;} return false; }
                if(nm=="utterly_inline" && e->args.size()==1){ uint64_t X; if(is_const_expr(e->args[0].get(),X)){ out=X; return true;} return false; }
                if((nm=="gt"||nm=="lt"||nm=="ge"||nm=="le"||nm=="eq"||nm=="ne") && e->args.size()==2){
                    uint64_t A,B; if(is_const_expr(e->args[0].get(),A)&&is_const_expr(e->args[1].get(),B)){
                        bool v=false;
                        if(nm=="gt") v=(A>B); else if(nm=="lt") v=(A<B);
                        else if(nm=="ge") v=(A>=B); else if(nm=="le") v=(A<=B);
                        else if(nm=="eq") v=(A==B); else if(nm=="ne") v=(A!=B);
                        out=v?1:0; return true;
                    }
                }
                return false;
            }
        }
        return false;
    }

    // rudimentary inference for implicit lets: arr if top-level call is arr_*
    Type::K infer_type(const Expr* e){
        if(e->kind==Expr::Call){
            if(e->name=="arr_new"||e->name=="arr_set"||e->name=="arr_of") return Type::Arr;
        }
        return Type::Int;
    }
};

// ----------------- IR
enum Op: uint8_t {
    PUSH_IMM64=0x01, ADD=0x02, DUP=0x06,
    STORE_LOCAL=0x10, LOAD_LOCAL=0x11,
    MAX_=0x30, MIN_=0x31,
    CMP_GT=0x32, CMP_LT=0x33, CMP_EQ=0x34, CMP_NE=0x35, CMP_GE=0x36, CMP_LE=0x37,
    ARR_NEW=0x40, ARR_GET=0x41, ARR_SET=0x42,
    JZ_ABS=0x70, JMP_ABS=0x71,
    RET=0x21
};

struct IRInstr{
    Op op;
    bool hasImm=false; uint64_t imm=0;     // for PUSH_IMM64
    bool hasIdx=false; uint16_t idx=0;     // for locals
    bool hasTarget=false; int target=-1;   // instr index target (for NASM labels)
};

struct Code{
    std::vector<IRInstr> seq;              // instruction sequence (for NASM labels)
    std::vector<uint8_t> bytes;            // linearized hex IR (with absolute byte targets)
};

static inline size_t instr_size(const IRInstr& I){
    switch(I.op){
        case PUSH_IMM64: return 1+8;
        case STORE_LOCAL: case LOAD_LOCAL: return 1+2;
        case JZ_ABS: case JMP_ABS: return 1+4;
        default: return 1;
    }
}

// ----------------- Emitter (with patches)
struct Emitter{
    Code code; Typer& T;
    struct FoldLog{ string what; int line; };
    std::vector<FoldLog> folds;

    int here() const { return (int)code.seq.size(); }
    int emit_raw(Op op){ code.seq.push_back({op}); return here()-1; }
    int emit_push(uint64_t v){ IRInstr I{PUSH_IMM64}; I.hasImm=true; I.imm=v; code.seq.push_back(I); return here()-1; }
    int emit_local(Op op,uint16_t i){ IRInstr I{op}; I.hasIdx=true; I.idx=i; code.seq.push_back(I); return here()-1; }
    int emit_jmp(Op op,int targetIdx=-1){ IRInstr I{op}; I.hasTarget=true; I.target=targetIdx; code.seq.push_back(I); return here()-1; }

    void patch_target(int at, int targetIdx){ code.seq[at].hasTarget=true; code.seq[at].target=targetIdx; }

    // ---- Expressions
    void gen_expr(const Expr* e){
        switch(e->kind){
            case Expr::Num: emit_push(e->val); break;
            case Expr::Var: {
                auto it=T.locals.find(e->name); if(it==T.locals.end()) throw std::runtime_error("use of undeclared "+e->name);
                emit_local(LOAD_LOCAL,(uint16_t)it->second.index);
            } break;
            case Expr::Add: gen_expr(e->a.get()); gen_expr(e->b.get()); emit_raw(ADD); break;
            case Expr::Call:{
                auto nm=e->name;
                if(nm=="max"||nm=="min"){
                    uint64_t CV; if(T.is_const_expr(e,CV)){ folds.push_back({"fold:"+nm,e->line}); emit_push(CV); }
                    else { if(e->args.size()!=2) throw std::runtime_error("max/min need 2 args");
                           gen_expr(e->args[0].get()); gen_expr(e->args[1].get()); emit_raw(nm=="max"?MAX_:MIN_); }
                } else if(nm=="ever_exact"){
                    if(e->args.size()!=1) throw std::runtime_error("ever_exact needs 1 arg");
                    uint64_t CV; if(T.is_const_expr(e->args[0].get(),CV)){ folds.push_back({"fold:ever_exact",e->line}); emit_push(CV); }
                    else { gen_expr(e->args[0].get()); }
                } else if(nm=="utterly_inline"){
                    if(e->args.size()!=1) throw std::runtime_error("utterly_inline needs 1 arg");
                    folds.push_back({"hint:inline",e->line}); gen_expr(e->args[0].get());
                } else if(nm=="gt"||nm=="lt"||nm=="ge"||nm=="le"||nm=="eq"||nm=="ne"){
                    if(e->args.size()!=2) throw std::runtime_error(nm+" needs 2 args");
                    uint64_t CV; if(T.is_const_expr(e,CV)){ emit_push(CV); }
                    else {
                        gen_expr(e->args[0].get()); gen_expr(e->args[1].get());
                        emit_raw( nm=="gt"?CMP_GT : nm=="lt"?CMP_LT : nm=="ge"?CMP_GE : nm=="le"?CMP_LE : nm=="eq"?CMP_EQ : CMP_NE );
                    }
                } else if(nm=="arr_new"){
                    if(e->args.size()!=1) throw std::runtime_error("arr_new(n) needs 1 arg");
                    gen_expr(e->args[0].get()); emit_raw(ARR_NEW);
                } else if(nm=="arr_get"){
                    if(e->args.size()!=2) throw std::runtime_error("arr_get(a,i) needs 2 args");
                    gen_expr(e->args[0].get()); gen_expr(e->args[1].get()); emit_raw(ARR_GET);
                } else if(nm=="arr_set"){
                    if(e->args.size()!=3) throw std::runtime_error("arr_set(a,i,v) needs 3 args");
                    gen_expr(e->args[0].get()); gen_expr(e->args[1].get()); gen_expr(e->args[2].get()); emit_raw(ARR_SET);
                } else if(nm=="arr_of"){
                    // arr_of(v0,v1,...)  => arr_new(len); then sets; arr_set returns ptr (so we can chain)
                    size_t len=e->args.size();
                    emit_push((uint64_t)len); emit_raw(ARR_NEW); // stack: ptr
                    for(size_t i=0;i<len;i++){
                        emit_raw(DUP);               // ptr, ptr
                        emit_push((uint64_t)i);      // ptr, ptr, i
                        gen_expr(e->args[i].get());  // ptr, ptr, i, vi
                        emit_raw(ARR_SET);           // -> ptr
                    }
                } else {
                    throw std::runtime_error("unknown call '"+nm+"'");
                }
            } break;
        }
    }

    // ---- Statements
    void gen_stmt(const Stmt& s){
        switch(s.kind){
            case Stmt::Let:{
                Type::K tk = (s.etype==Stmt::T_Int)?Type::Int : (s.etype==Stmt::T_Arr)?Type::Arr : T.infer_type(s.expr.get());
                bool explicitType=(s.etype!=Stmt::T_Implicit);
                T.declare_local(s.name,s.line,explicitType,tk);
                gen_expr(s.expr.get());
                emit_local(STORE_LOCAL,(uint16_t)T.locals.at(s.name).index);
            } break;
            case Stmt::Ret:{ gen_expr(s.expr.get()); emit_raw(RET); } break;
            case Stmt::If:{
                gen_expr(s.cond.get());
                int jz=emit_jmp(JZ_ABS,-1);
                for(auto& st:s.thenBody) gen_stmt(st);
                int jmpEnd=emit_jmp(JMP_ABS,-1);
                int elseAt=here();
                patch_target(jz, elseAt);
                for(auto& st:s.elseBody) gen_stmt(st);
                int endAt=here();
                patch_target(jmpEnd, endAt);
            } break;
        }
    }

    void gen_func(const Func& f){ for(auto& s:f.body) gen_stmt(s); }

    // ---- finalize bytes with absolute targets
    void finalize_bytes(){
        // map instr index -> byte offset
        std::vector<uint32_t> off(code.seq.size()+1,0);
        for(size_t i=0;i<code.seq.size();++i) off[i+1] = off[i] + (uint32_t)instr_size(code.seq[i]);
        code.bytes.clear(); code.bytes.reserve(off.back());
        auto out_u8=[&](uint8_t v){ code.bytes.push_back(v); };
        auto out_u16=[&](uint16_t v){ code.bytes.push_back((uint8_t)(v&0xFF)); code.bytes.push_back((uint8_t)((v>>8)&0xFF)); };
        auto out_u32=[&](uint32_t v){ code.bytes.push_back((uint8_t)(v&0xFF)); code.bytes.push_back((uint8_t)((v>>8)&0xFF)); code.bytes.push_back((uint8_t)((v>>16)&0xFF)); code.bytes.push_back((uint8_t)((v>>24)&0xFF)); };
        auto out_u64=[&](uint64_t v){ for(int i=0;i<8;i++) code.bytes.push_back((uint8_t)((v>>(i*8))&0xFF)); };

        for(size_t i=0;i<code.seq.size();++i){
            const auto& I=code.seq[i];
            out_u8((uint8_t)I.op);
            switch(I.op){
                case PUSH_IMM64: out_u64(I.imm); break;
                case STORE_LOCAL: case LOAD_LOCAL: out_u16(I.idx); break;
                case JZ_ABS: case JMP_ABS:{
                    uint32_t tgt = I.hasTarget? off[(size_t)I.target] : 0;
                    out_u32(tgt);
                } break;
                default: break;
            }
        }
    }
};

// ----------------- Range/Capsules (simple guard)
struct RangeContext{ string current="app"; std::vector<string> stack; void enter(const string&r){ stack.push_back(current); current=r; } void leave(){ if(!stack.empty()){ current=stack.back(); stack.pop_back(); } } };
static RangeContext gRange;

struct CapsuleArena{ std::vector<uint8_t> buf; size_t off=0; string range; explicit CapsuleArena(size_t cap=1<<20,string r="app"):buf(cap),range(std::move(r)){} void* alloc(size_t n){ if(off+n>buf.size()) throw std::bad_alloc(); void* p=&buf[off]; off+=n; return p;} void reset(){off=0;} };

template<class T> struct CapsuleHandle{
    T* ptr=nullptr; size_t count=0; string range;
    T& operator[](size_t i){
        if(range!=gRange.current) throw std::runtime_error("Capsule range violation");
        if(i>=count) throw std::out_of_range("Capsule index OOB");
        return ptr[i];
    }
};
template<class T> static CapsuleHandle<T> capsule_alloc(CapsuleArena&A,size_t n){ auto p=reinterpret_cast<T*>(A.alloc(n*sizeof(T))); for(size_t i=0;i<n;i++) new(&p[i])T(); return CapsuleHandle<T>{p,n,A.range}; }

// ----------------- VM (with arrays)
struct VM{
    const std::vector<uint8_t>& b; std::vector<int64_t> stack; std::vector<int64_t> locals;
    // array heap: id -> vector<int64_t>
    std::vector<std::vector<int64_t>> arrays;

    explicit VM(const std::vector<uint8_t>& bytes,int localCount):b(bytes),locals(localCount,0){}
    inline uint32_t get_u32(size_t& ip){ uint32_t v=b[ip]|(b[ip+1]<<8)|(b[ip+2]<<16)|(b[ip+3]<<24); ip+=4; return v; }
    inline uint16_t get_u16(size_t& ip){ uint16_t v=b[ip]|(b[ip+1]<<8); ip+=2; return v; }
    inline uint64_t get_u64(size_t& ip){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)b[ip+i]<<(i*8); ip+=8; return v; }

    int64_t run_all(){
        size_t ip=0;
        for(;;){
            if(ip>=b.size()) throw std::runtime_error("VM OOB");
            switch((Op)b[ip++]){
                case PUSH_IMM64:{ auto v=get_u64(ip); stack.push_back((int64_t)v);} break;
                case LOAD_LOCAL:{ auto idx=get_u16(ip); stack.push_back(locals[idx]); } break;
                case STORE_LOCAL:{ auto idx=get_u16(ip); auto v=stack.back(); stack.pop_back(); locals[idx]=v; } break;
                case DUP:{ auto v=stack.back(); stack.push_back(v);} break;
                case ADD:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back(ra+rb);} break;
                case MAX_:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra>rb)?ra:rb ); } break;
                case MIN_:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra<rb)?ra:rb ); } break;
                case CMP_GT:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra>rb)?1:0 ); } break;
                case CMP_LT:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra<rb)?1:0 ); } break;
                case CMP_EQ:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra==rb)?1:0 ); } break;
                case CMP_NE:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra!=rb)?1:0 ); } break;
                case CMP_GE:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra>=rb)?1:0 ); } break;
                case CMP_LE:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra<=rb)?1:0 ); } break;
                case ARR_NEW:{ auto len=stack.back(); stack.pop_back(); if(len<0) len=0; arrays.push_back(std::vector<int64_t>((size_t)len,0)); int64_t id=(int64_t)arrays.size(); stack.push_back(id); } break;
                case ARR_GET:{ auto idx=stack.back(); stack.pop_back(); auto id=stack.back(); stack.pop_back(); int64_t v=0; if(id>0 && (size_t)id<=arrays.size()){ auto& a=arrays[(size_t)id-1]; if(idx>=0 && (size_t)idx<a.size()) v=a[(size_t)idx]; } stack.push_back(v); } break;
                case ARR_SET:{ auto v=stack.back(); stack.pop_back(); auto idx=stack.back(); stack.pop_back(); auto id=stack.back(); stack.pop_back(); if(id>0 && (size_t)id<=arrays.size()){ auto& a=arrays[(size_t)id-1]; if(idx>=0 && (size_t)idx<a.size()) a[(size_t)idx]=v; } stack.push_back(id); } break;
                case JZ_ABS:{ auto tgt=get_u32(ip); auto v=stack.back(); stack.pop_back(); if(v==0) ip=tgt; } break;
                case JMP_ABS:{ auto tgt=get_u32(ip); ip=tgt; } break;
                case RET:{ auto v=stack.back(); return v; }
                default: throw std::runtime_error("VM bad opcode");
            }
        }
    }
};

// ----------------- NASM(PE) emitter (covers arrays + cmp + jcc)
struct NASM{
    std::ostringstream asmtext;
    int labelCounter=0;
    std::unordered_map<int,string> labelForInstr; // IR index -> label

    string mkLabel(){ return ".L"+std::to_string(labelCounter++); }
    const string& ensureLabel(int instrIndex){
        auto it=labelForInstr.find(instrIndex);
        if(it!=labelForInstr.end()) return it->second;
        auto lab=mkLabel(); auto res=labelForInstr.emplace(instrIndex,lab);
        return res.first->second;
    }
    // Windows x64 prologue with proper alignment + heap init
    void prologue(int locals, bool needsHeap){
        asmtext<<"default rel\nextern ExitProcess\nextern GetProcessHeap\nextern HeapAlloc\n";
        asmtext<<"section .text\n";
        asmtext<<"global main\n";
        asmtext<<"main:\n";
        asmtext<<"    push rbp\n";
        asmtext<<"    mov rbp, rsp\n";
        int shadow=32;
        int reserve = locals*8 + shadow;
        reserve = (reserve + 15) & ~15; // align to 16
        asmtext<<"    sub rsp, "<<reserve<<"\n";
        if(needsHeap){
            // RCX...; call GetProcessHeap -> RAX, save in r12 (non-volatile)
            asmtext<<"    call GetProcessHeap\n";
            asmtext<<"    mov r12, rax\n";
        }
    }
    void epilogue(){
        asmtext<<"    mov ecx, eax\n";
        asmtext<<"    call ExitProcess\n";
    }
    void placeLabel(const string& L){ asmtext<<L<<":\n"; }

    // stack helpers
    void op_push_imm(uint64_t v){ asmtext<<"    mov rax, 0x"<<std::hex<<v<<std::dec<<"\n    push rax\n"; }
    void op_dup(){ asmtext<<"    mov rax, [rsp]\n    push rax\n"; }
    void op_load_local(int idx){ int off=(idx+1)*8; asmtext<<"    mov rax, [rbp - "<<off<<"]\n    push rax\n"; }
    void op_store_local(int idx){ int off=(idx+1)*8; asmtext<<"    pop rax\n    mov [rbp - "<<off<<"], rax\n"; }
    void op_add(){ asmtext<<"    pop rbx\n    pop rax\n    add rax, rbx\n    push rax\n"; }
    void op_max(){ asmtext<<"    pop rbx\n    pop rax\n    cmp rax, rbx\n    cmovl rax, rbx\n    push rax\n"; }
    void op_min(){ asmtext<<"    pop rbx\n    pop rax\n    cmp rax, rbx\n    cmovg rax, rbx\n    push rax\n"; }
    void op_cmp_setcc(const char* cc){ // push 0/1
        asmtext<<"    pop rbx\n    pop rax\n    cmp rax, rbx\n    set"<<cc<<" al\n    movzx rax, al\n    push rax\n";
    }
    void op_ret(){ asmtext<<"    pop rax\n"; }
    void op_jz(const string& L){ asmtext<<"    pop rax\n    test rax, rax\n    jz "<<L<<"\n"; }
    void op_jmp(const string& L){ asmtext<<"    jmp "<<L<<"\n"; }

    // arrays: r12 holds process heap handle
    void op_arr_new(){
        // stack: [ ... len ]
        // => rbx=len; size = len*8 + 8; HeapAlloc(r12,0,size) -> rax; [rax]=len; push rax
        asmtext<<"    pop rbx\n";                             // len
        asmtext<<"    lea r8, [rbx*8 + 8]\n";                 // size bytes
        asmtext<<"    mov rcx, r12\n";                        // heap
        asmtext<<"    xor rdx, rdx\n";                        // flags 0
        asmtext<<"    call HeapAlloc\n";                      // rax = ptr
        asmtext<<"    mov [rax], rbx\n";                      // store length
        asmtext<<"    push rax\n";                            // push ptr (base at len field)
    }
    void op_arr_get(){
        // stack: [... ptr, idx]  (we emitted ptr then idx) → but our VM pushes ptr then idx, IR arr_get consumed ptr then idx
        // our IR eval order produced: push ptr; push idx; so top is idx (rbx), then ptr (rax)
        asmtext<<"    pop rbx\n";               // idx
        asmtext<<"    pop rax\n";               // ptr
        // bounds: len=[rax]
        asmtext<<"    mov rcx, [rax]\n";
        asmtext<<"    cmp rbx, rcx\n";
        string ok = mkLabel(), done=mkLabel();
        asmtext<<"    jae "<<ok<<"\n"; // if idx >= len -> OOB
        // in bounds
        asmtext<<"    mov rdx, [rax + 8 + rbx*8]\n";
        asmtext<<"    push rdx\n";
        asmtext<<"    jmp "<<done<<"\n";
        // OOB -> push 0
        placeLabel(ok);
        asmtext<<"    xor rdx, rdx\n    push rdx\n";
        placeLabel(done);
    }
    void op_arr_set(){
        // stack: [... ptr, idx, val]  (we emitted ptr, idx, val)  top is val (rdx), below idx (rbx), below ptr (rax)
        asmtext<<"    pop rdx\n";               // val
        asmtext<<"    pop rbx\n";               // idx
        asmtext<<"    pop rax\n";               // ptr
        asmtext<<"    mov rcx, [rax]\n";        // len
        asmtext<<"    cmp rbx, rcx\n";
        string ok = mkLabel();
        // if OOB, ignore; return ptr
        asmtext<<"    jae "<<ok<<"\n";
        asmtext<<"    mov [rax + 8 + rbx*8], rdx\n";
        placeLabel(ok);
        asmtext<<"    push rax\n";              // arr_set returns ptr (chainable)
    }
};

static void emit_nasm_pe(const Code& code, int localCount, const string& outdir){
    // Determine if arrays are used to add heap init
    bool needsHeap=false;
    for(auto& I: code.seq) if(I.op==ARR_NEW||I.op==ARR_GET||I.op==ARR_SET) { needsHeap=true; break; }

    NASM n;
    n.prologue(localCount, needsHeap);

    // Mark labels for branch targets
    for(size_t i=0;i<code.seq.size();++i){
        const auto& I=code.seq[i];
        if((I.op==JZ_ABS||I.op==JMP_ABS) && I.hasTarget) n.ensureLabel(I.target);
    }

    // Emit instructions and labels
    for(size_t i=0;i<code.seq.size();++i){
        if(n.labelForInstr.count((int)i)) n.placeLabel(n.labelForInstr[(int)i]);
        const auto& I=code.seq[i];
        switch(I.op){
            case PUSH_IMM64: n.op_push_imm(I.imm); break;
            case LOAD_LOCAL: n.op_load_local(I.idx); break;
            case STORE_LOCAL: n.op_store_local(I.idx); break;
            case DUP: n.op_dup(); break;
            case ADD: n.op_add(); break;
            case MAX_: n.op_max(); break;
            case MIN_: n.op_min(); break;
            case CMP_GT: n.op_cmp_setcc("g"); break;
            case CMP_LT: n.op_cmp_setcc("l"); break;
            case CMP_EQ: n.op_cmp_setcc("e"); break;
            case CMP_NE: n.op_cmp_setcc("ne"); break;
            case CMP_GE: n.op_cmp_setcc("ge"); break;
            case CMP_LE: n.op_cmp_setcc("le"); break;
            case ARR_NEW: n.op_arr_new(); break;
            case ARR_GET: n.op_arr_get(); break;
            case ARR_SET: n.op_arr_set(); break;
            case JZ_ABS:{
                string L = n.ensureLabel(I.target);
                n.op_jz(L);
            } break;
            case JMP_ABS:{
                string L = n.ensureLabel(I.target);
                n.op_jmp(L);
            } break;
            case RET: n.op_ret(); goto end_emit;
            default: throw std::runtime_error("NASM emitter: bad opcode");
        }
    }
end_emit:
    n.epilogue();

    // write files
#ifdef _WIN32
    std::string cmd="if not exist \""+outdir+"\" mkdir \""+outdir+"\"";
    system(cmd.c_str());
#endif
    std::string asmPath=outdir+"/parashade_main.asm";
    std::ofstream a(asmPath,std::ios::binary); a<<n.asmtext.str(); a.close();

    // build.bat
    std::string batPath=outdir+"/build.bat";
    std::ofstream b(batPath,std::ios::binary);
    b<<R"(REM Build PE from NASM with MSVC LINK
@echo off
setlocal
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  echo (Tip) Use the "x64 Native Tools Command Prompt for VS" so link.exe is on PATH.
)
if "%1"=="" ( set OUT=parashade.exe ) else ( set OUT=%1 )
echo Assembling...
nasm -f win64 parashade_main.asm -o parashade_main.obj || exit /b 1
echo Linking...
link /subsystem:console /entry:main parashade_main.obj kernel32.lib || exit /b 1
echo Done: %OUT%
)";
    b.close();
}

// ----------------- HEX + metadata
static string hex_dump(const std::vector<uint8_t>& v){
    std::ostringstream o; o<<std::hex<<std::setfill('0');
    for(size_t i=0;i<v.size();++i){ if(i%16==0) o<<"\n"; o<<std::setw(2)<<(int)v[i]<<' '; }
    return o.str();
}

static string meta_json(const Module& m, const Typer& T, const Emitter& E){
    // locals sorted by index
    std::vector<const Local*> locs; locs.reserve(T.locals.size());
    for(auto& kv:T.locals) locs.push_back(&kv.second);
    std::sort(locs.begin(),locs.end(),[](auto* a, auto* b){ return a->index<b->index; });

    std::ostringstream s;
    s<<"{\n";
    s<<"  \"module\":\""<<m.name<<"\",\n";
    s<<"  \"functions\":[{\"name\":\""<<m.mainFn.name<<"\",\"locals\":[";
    for(size_t i=0;i<locs.size();++i){
        if(i) s<<",";
        s<<"{\"name\":\""<<locs[i]->name<<"\",\"type\":\""<<(locs[i]->ty.k==Type::Int?"int":"arr")
         <<"\",\"index\":"<<locs[i]->index<<",\"line\":"<<locs[i]->declLine
         <<",\"explicit\":"<<(locs[i]->explicitDeclared?"true":"false")<<"}";
    }
    s<<"]}],\n";
    s<<"  \"warnings\":[";
    bool first=true;
    for(auto& w:T.warns){ if(!first) s<<","; first=false; s<<"{\"code\":\""<<w.code<<"\",\"line\":"<<w.line<<",\"msg\":\""<<w.msg<<"\"}"; }
    for(auto& f:E.folds){ if(!first) s<<","; first=false; s<<"{\"code\":\"W100\",\"line\":"<<f.line<<",\"msg\":\""<<f.what<<"\"}"; }
    s<<"]\n";
    s<<"}\n";
    return s.str();
}

// ----------------- Driver
int main(int argc, char** argv){
    std::ios::sync_with_stdio(false); std::cin.tie(nullptr);

    bool run=false, emit=false, emit_nasm=false; string outdir=".";
    for(int i=1;i<argc;i++){
        string a=argv[i];
        if(a=="--run") run=true;
        else if(a=="--emit") emit=true;
        else if(a=="--emit-nasm"){ emit_nasm=true; if(i+1<argc) outdir=argv[++i]; }
    }

    string src((std::istreambuf_iterator<char>(std::cin)), {});
    string norm=normalize_longform(src);

    try{
        Lexer L(norm); Parser P(L); Module mod=P.parseModule();
        Typer T; Emitter E(T); E.gen_func(mod.mainFn); E.finalize_bytes();

        if(run){
            VM vm(E.code.bytes,(int)T.locals.size());
            auto ret=vm.run_all();
            std::cout<<ret<<"\n";
            return 0;
        }
        if(emit){
            std::cout<<"; PARASHADE v0.3 HEX IR ("<<E.code.bytes.size()<<" bytes)\n";
            std::cout<<hex_dump(E.code.bytes)<<"\n\n; METADATA\n"<<meta_json(mod,T,E);
            return 0;
        }
        if(emit_nasm){
            emit_nasm_pe(E.code,(int)T.locals.size(),outdir);
            std::cout<<"Wrote "<<outdir<<"/parashade_main.asm and build.bat\n";
            return 0;
        }
        std::cerr<<"Usage: --run | --emit | --emit-nasm <outdir>\n";
        return 1;
    } catch(const std::exception& e){
        std::cerr<<"Compile/Run error: "<<e.what()<<"\n";
        return 2;
    }
}
