// parashade_win.cpp — Parashade v0.2 (Windows-focused)
// MSVC:   cl /std:c++17 /O2 /EHsc /Fe:parashade.exe parashade_win.cpp
// Clang:  clang-cl /std:c++17 /O2 /EHsc /Fe:parashade.exe parashade_win.cpp
// Usage:  type file.psd | parashade.exe --run
//         type file.psd | parashade.exe --emit
//         type file.psd | parashade.exe --emit-nasm .out
//
// What’s new vs seed:
// - NASM emitter (PE/COFF) + build.bat (nasm + link.exe) for a real Windows .exe
// - Range-checked Capsules with typed handles and simple runtime guards
// - Superlatives: max/min/ever_exact/utterly_inline
//   * compile-time folding (constant fold) + IR ops MAX/MIN if not constant
// - Expanded warnings into .meta.json (implicit lets, folds, inline hints)
//
// Notes: This is a compact educational compiler+VM+NASM emitter for a teeny subset.
//        It’s intentionally simple and safe to extend.

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

static inline string trim(string s){
    size_t a=0; while(a<s.size() && isspace((unsigned char)s[a]))++a;
    size_t b=s.size(); while(b>a && isspace((unsigned char)s[b-1]))--b;
    return s.substr(a,b-a);
}
static inline bool starts_with(const string& s, const string& p){ return s.rfind(p,0)==0; }
static inline string lowerc(string s){ for(char& c:s) c=char(tolower((unsigned char)c)); return s; }

// ---------- Long-form → Core normalizer
static string normalize_longform(const string& in){
    // Map verbose phrases to compact core tokens; keep lines NASM-like.
    static const std::vector<std::pair<std::regex,string>> M = {
        {std::regex("\\bdeclare\\s+explicit\\s+integer\\s+named\\s+"), "let int "},
        {std::regex("\\bdeclare\\s+implicit\\s+named\\s+"),            "let "},
        {std::regex("\\bequals\\b"),                                   "="},
        {std::regex("\\bend\\b"),                                      ""}, // end-of-decl
        {std::regex("\\bplus\\b"),                                     "+"},
        {std::regex("\\bmodule\\b"),                                   "module"},
        {std::regex("\\bscope\\b"),                                    "scope"},
        {std::regex("\\brange\\b"),                                    "range"},
        {std::regex("\\breturn\\b"),                                   "return"},
        // Encourage function-call style superlatives:
        // greatest_of a and b  → recommend writing max(a,b) directly in source for now.
    };
    std::ostringstream out;
    std::istringstream iss(in); string line;
    while(std::getline(iss,line)){
        auto sc = line.find(';'); if(sc!=string::npos) line = line.substr(0,sc);
        string L = line;
        for (auto& p : M) L = std::regex_replace(L, p.first, p.second);
        out << trim(L) << "\n";
    }
    return out.str();
}

// ---------- Lexer (NASM-ish tokens)
enum class Tok {
    End, Ident, Number,
    Colon, Equals, Plus, Comma,
    LParen, RParen,
    KwModule, KwScope, KwRange, KwLet, KwInt, KwReturn, KwEnd
};
struct Token { Tok t; string s; int line; };

struct Lexer {
    std::vector<Token> toks; size_t i=0;
    explicit Lexer(const string& src){
        std::istringstream iss(src); string line; int ln=0;
        while(std::getline(iss,line)){
            ++ln;
            auto sc=line.find(';'); if(sc!=string::npos) line=line.substr(0,sc);
            std::istringstream ls(line); string w;
            for(;;){
                int c = ls.peek();
                if(c==EOF) break;
                if(isspace(c)){ ls.get(); continue; }
                if(c=='('){ toks.push_back({Tok::LParen,"(",ln}); ls.get(); continue; }
                if(c==')'){ toks.push_back({Tok::RParen,")",ln}); ls.get(); continue; }
                if(c==','){ toks.push_back({Tok::Comma,",",ln}); ls.get(); continue; }
                if(c==':'){ toks.push_back({Tok::Colon,":",ln}); ls.get(); continue; }
                if(c=='='){ toks.push_back({Tok::Equals,"=",ln}); ls.get(); continue; }
                if(c=='+'){ toks.push_back({Tok::Plus,"+",ln}); ls.get(); continue; }

                // word/number
                if(std::isalpha(c) || c=='_' ){
                    string id;
                    while(std::isalnum(ls.peek()) || ls.peek()=='_' ) id.push_back(char(ls.get()));
                    string lid = lowerc(id);
                    if(lid=="module") toks.push_back({Tok::KwModule,id,ln});
                    else if(lid=="scope") toks.push_back({Tok::KwScope,id,ln});
                    else if(lid=="range") toks.push_back({Tok::KwRange,id,ln});
                    else if(lid=="let") toks.push_back({Tok::KwLet,id,ln});
                    else if(lid=="int") toks.push_back({Tok::KwInt,id,ln});
                    else if(lid=="return") toks.push_back({Tok::KwReturn,id,ln});
                    else if(lid=="end") toks.push_back({Tok::KwEnd,id,ln});
                    else toks.push_back({Tok::Ident,id,ln});
                    continue;
                } else {
                    // number? accept hex 0x..
                    if(c=='0'){
                        string num; num.push_back(char(ls.get()));
                        if(lowerc(string(1,char(ls.peek())))=="x"){ num.push_back(char(ls.get()));
                            while(std::isxdigit(ls.peek()) || ls.peek()=='_') num.push_back(char(ls.get()));
                            toks.push_back({Tok::Number,num,ln}); continue;
                        } else {
                            // fall through: treat 0 as decimal zero literal
                            while(std::isdigit(ls.peek())) num.push_back(char(ls.get()));
                            toks.push_back({Tok::Number,num,ln}); continue;
                        }
                    }
                    // decimal
                    if(std::isdigit(c)){
                        string num;
                        while(std::isdigit(ls.peek())) num.push_back(char(ls.get()));
                        toks.push_back({Tok::Number,num,ln}); continue;
                    }
                    // unknown
                    ls.get(); // skip
                }
            }
        }
        toks.push_back({Tok::End,"", ln});
    }
    const Token& peek() const { return toks[i]; }
    Token pop(){ return toks[i++]; }
    bool accept(Tok t){ if(peek().t==t){ ++i; return true; } return false; }
    void expect(Tok t, const char* msg){
        if(!accept(t)) throw std::runtime_error(string("Parse error: expected ")+msg+" at line "+std::to_string(peek().line));
    }
};

// ---------- AST
struct Expr {
    enum Kind { Num, Var, Add, Call } kind;
    int line=0;
    uint64_t val=0;           // for Num
    string name;              // Var or Call name
    std::unique_ptr<Expr> a,b;         // for Add
    std::vector<std::unique_ptr<Expr>> args; // for Call
    static std::unique_ptr<Expr> num(uint64_t v, int ln){ auto p=std::make_unique<Expr>(); p->kind=Num; p->val=v; p->line=ln; return p; }
    static std::unique_ptr<Expr> var(string n, int ln){ auto p=std::make_unique<Expr>(); p->kind=Var; p->name=std::move(n); p->line=ln; return p; }
    static std::unique_ptr<Expr> add(std::unique_ptr<Expr> A, std::unique_ptr<Expr> B, int ln){
        auto p=std::make_unique<Expr>(); p->kind=Add; p->a=std::move(A); p->b=std::move(B); p->line=ln; return p;
    }
    static std::unique_ptr<Expr> call(string n, std::vector<std::unique_ptr<Expr>> as, int ln){
        auto p=std::make_unique<Expr>(); p->kind=Call; p->name=std::move(n); p->args=std::move(as); p->line=ln; return p;
    }
};

struct Stmt {
    enum Kind { Let, Ret } kind;
    int line=0;
    string name; bool explicitInt=false; // for Let
    std::unique_ptr<Expr> expr;
    static Stmt makeLet(string n, bool expl, std::unique_ptr<Expr> e, int ln){
        Stmt s; s.kind=Let; s.name=std::move(n); s.explicitInt=expl; s.expr=std::move(e); s.line=ln; return s;
    }
    static Stmt makeRet(std::unique_ptr<Expr> e, int ln){ Stmt s; s.kind=Ret; s.expr=std::move(e); s.line=ln; return s; }
};

struct Func { string name; int line=0; std::vector<Stmt> body; };
struct Module { string name; Func mainFn; };

// ---------- Parser
struct Parser {
    Lexer& L;
    explicit Parser(Lexer& l):L(l){}
    Module parseModule(){
        L.expect(Tok::KwModule,"module");
        auto id=L.pop(); if(id.t!=Tok::Ident) throw std::runtime_error("module: expected identifier");
        L.expect(Tok::Colon,":");
        Module m; m.name=id.s;
        m.mainFn = parseScope();
        return m;
    }
    Func parseScope(){
        L.expect(Tok::KwScope,"scope");
        auto id=L.pop(); if(id.t!=Tok::Ident || lowerc(id.s)!="main") throw std::runtime_error("only 'scope main' supported");
        L.expect(Tok::KwRange,"range"); auto r=L.pop(); if(r.t!=Tok::Ident) throw std::runtime_error("range: expected identifier");
        L.expect(Tok::Colon,":");
        Func f; f.name="main"; f.line=id.line;
        while(L.peek().t!=Tok::KwEnd && L.peek().t!=Tok::End){
            f.body.push_back(parseStmt());
        }
        L.expect(Tok::KwEnd,"end");
        return f;
    }
    Stmt parseStmt(){
        if(L.peek().t==Tok::KwLet){
            auto letTok=L.pop(); bool expl=false;
            if(L.accept(Tok::KwInt)) expl=true;
            auto id=L.pop(); if(id.t!=Tok::Ident) throw std::runtime_error("let: expected name");
            L.expect(Tok::Equals,"=");
            auto e=parseExpr();
            return Stmt::makeLet(id.s, expl, std::move(e), letTok.line);
        }
        if(L.peek().t==Tok::KwReturn){
            auto rt=L.pop(); auto e=parseExpr();
            return Stmt::makeRet(std::move(e), rt.line);
        }
        throw std::runtime_error("Unknown statement at line "+std::to_string(L.peek().line));
    }
    std::unique_ptr<Expr> parseExpr(){
        auto t=parsePrimary();
        while(L.accept(Tok::Plus)){
            auto r=parsePrimary();
            t=Expr::add(std::move(t), std::move(r), r->line);
        }
        return t;
    }
    std::unique_ptr<Expr> parsePrimary(){
        auto tk=L.pop();
        if(tk.t==Tok::Number){
            uint64_t v=0;
            if(starts_with(tk.s,"0x")||starts_with(tk.s,"0X")){
                std::string s = tk.s.substr(2);
                s.erase(std::remove(s.begin(), s.end(),'_'), s.end());
                std::stringstream ss; ss<<std::hex<<s; ss>>v;
            } else {
                std::stringstream ss; ss<<tk.s; ss>>v;
            }
            return Expr::num(v, tk.line);
        } else if(tk.t==Tok::Ident){
            // call?
            if(L.accept(Tok::LParen)){
                std::vector<std::unique_ptr<Expr>> args;
                if(L.peek().t!=Tok::RParen){
                    args.push_back(parseExpr());
                    while(L.accept(Tok::Comma)) args.push_back(parseExpr());
                }
                L.expect(Tok::RParen,")");
                return Expr::call(lowerc(tk.s), std::move(args), tk.line);
            }
            return Expr::var(lowerc(tk.s), tk.line);
        } else if(tk.t==Tok::LParen){
            auto e=parseExpr(); L.expect(Tok::RParen,")"); return e;
        }
        throw std::runtime_error("Expected primary at line "+std::to_string(tk.line));
    }
};

// ---------- Types / Locals
struct Type { enum K { Int } k; };
struct Local { string name; Type ty; int index; int declLine; bool explicitInt; };

struct Typer {
    std::unordered_map<string,Local> locals;
    int nextIdx=0;
    struct Warning { string code,msg; int line; };
    std::vector<Warning> warns;

    void declare_local(const string& n, int line, bool explicitInt){
        if(!locals.count(n)){
            locals[n] = Local{n, Type{Type::Int}, nextIdx++, line, explicitInt};
            if(!explicitInt) warns.push_back({"W001","implicit integer type inferred for '"+n+"'", line});
        }
    }
    bool is_const_expr(const Expr* e, uint64_t& out) const {
        switch(e->kind){
            case Expr::Num: out=e->val; return true;
            case Expr::Var: return false;
            case Expr::Add: {
                uint64_t A,B; if(is_const_expr(e->a.get(),A) && is_const_expr(e->b.get(),B)){ out = A + B; return true; }
                return false;
            }
            case Expr::Call: {
                if(e->name=="ever_exact" && e->args.size()==1){
                    uint64_t X; if(is_const_expr(e->args[0].get(),X)){ out=X; return true; }
                    return false;
                }
                if((e->name=="max"||e->name=="min") && e->args.size()==2){
                    uint64_t A,B; if(is_const_expr(e->args[0].get(),A) && is_const_expr(e->args[1].get(),B)){
                        out = (e->name=="max") ? (std::max<uint64_t>(A,B)) : (std::min<uint64_t>(A,B));
                        return true;
                    }
                }
                if(e->name=="utterly_inline" && e->args.size()==1){
                    uint64_t X; if(is_const_expr(e->args[0].get(),X)){ out=X; return true; }
                    return false;
                }
                return false;
            }
        }
        return false;
    }

    void check(const Func& f){
        (void)f; // this tiny demo only has ints
    }
};

// ---------- IR
enum Op: uint8_t {
    PUSH_IMM64=0x01, ADD=0x02,
    STORE_LOCAL=0x10, LOAD_LOCAL=0x11,
    MAX_=0x30, MIN_=0x31,
    RET=0x21
};
struct Code { std::vector<uint8_t> bytes; };

static inline void emit_u8(Code& c, uint8_t v){ c.bytes.push_back(v); }
static inline void emit_u16(Code& c, uint16_t v){ c.bytes.push_back((uint8_t)(v&0xFF)); c.bytes.push_back((uint8_t)((v>>8)&0xFF)); }
static inline void emit_u64(Code& c, uint64_t v){ for(int i=0;i<8;i++) c.bytes.push_back((uint8_t)((v>>(i*8))&0xFF)); }

struct Emitter {
    Code code; Typer& T;
    struct FoldLog { string what; int line; };
    std::vector<FoldLog> folds;
    Emitter(Typer& t):T(t){}
    void gen_expr(const Expr* e){
        switch(e->kind){
            case Expr::Num: emit_u8(code,PUSH_IMM64); emit_u64(code,e->val); break;
            case Expr::Var: emit_u8(code,LOAD_LOCAL); emit_u16(code,(uint16_t)T.locals.at(e->name).index); break;
            case Expr::Add: gen_expr(e->a.get()); gen_expr(e->b.get()); emit_u8(code,ADD); break;
            case Expr::Call: {
                if(e->name=="max"||e->name=="min"){
                    uint64_t CV;
                    if(T.is_const_expr(e, CV)){
                        folds.push_back({"fold:"+e->name, e->line});
                        emit_u8(code,PUSH_IMM64); emit_u64(code,CV);
                    } else {
                        if(e->args.size()!=2) throw std::runtime_error("max/min need 2 args");
                        gen_expr(e->args[0].get());
                        gen_expr(e->args[1].get());
                        emit_u8(code, (e->name=="max")?MAX_:MIN_);
                    }
                } else if(e->name=="ever_exact"){
                    if(e->args.size()!=1) throw std::runtime_error("ever_exact needs 1 arg");
                    uint64_t CV;
                    if(T.is_const_expr(e->args[0].get(), CV)){
                        folds.push_back({"fold:ever_exact", e->line});
                        emit_u8(code,PUSH_IMM64); emit_u64(code,CV);
                    } else {
                        // identity
                        gen_expr(e->args[0].get());
                    }
                } else if(e->name=="utterly_inline"){
                    if(e->args.size()!=1) throw std::runtime_error("utterly_inline needs 1 arg");
                    // identity + annotate
                    folds.push_back({"hint:inline", e->line});
                    gen_expr(e->args[0].get());
                } else {
                    throw std::runtime_error("unknown call '"+e->name+"'");
                }
            } break;
        }
    }
    void gen_func(const Func& f){
        for(auto& s: f.body){
            if(s.kind==Stmt::Let){
                T.declare_local(s.name, s.line, s.explicitInt);
                gen_expr(s.expr.get());
                emit_u8(code,STORE_LOCAL); emit_u16(code,(uint16_t)T.locals.at(s.name).index);
            } else if(s.kind==Stmt::Ret){
                gen_expr(s.expr.get());
                emit_u8(code,RET);
            }
        }
    }
};

// ---------- Capsules (range-checked typed handles)
struct RangeContext {
    string current = "app";
    std::vector<string> stack;
    void enter(const string& r){ stack.push_back(current); current=r; }
    void leave(){ if(!stack.empty()){ current=stack.back(); stack.pop_back(); } }
};
static RangeContext gRange;

struct CapsuleArena {
    std::vector<uint8_t> buf; size_t off=0; string range;
    explicit CapsuleArena(size_t cap=1<<20, string r="app"):buf(cap),range(std::move(r)){}
    void* alloc(size_t n){
        if(off+n>buf.size()) throw std::bad_alloc();
        void* p = &buf[off]; off += n; return p;
    }
    void reset(){ off=0; }
};

template<typename T>
struct CapsuleHandle {
    T* ptr=nullptr; size_t count=0; string range;
    T& operator[](size_t i){
        if(range!=gRange.current) throw std::runtime_error("Capsule range violation: access from '"+gRange.current+"' but owned by '"+range+"'");
        if(i>=count) throw std::out_of_range("Capsule index OOB");
        return ptr[i];
    }
};

template<typename T>
static CapsuleHandle<T> capsule_alloc(CapsuleArena& A, size_t n){
    auto p = reinterpret_cast<T*>(A.alloc(sizeof(T)*n));
    for(size_t i=0;i<n;i++) new (&p[i]) T();
    return CapsuleHandle<T>{p,n,A.range};
}

// ---------- VM (AOT) and frame interpreter
struct VM {
    const std::vector<uint8_t>& b; std::vector<int64_t> stack; std::vector<int64_t> locals;
    explicit VM(const std::vector<uint8_t>& bytes, int localCount):b(bytes), locals(localCount,0){}
    int64_t run_all(){
        size_t ip=0;
        for(;;){
            if(ip>=b.size()) throw std::runtime_error("VM OOB");
            switch(b[ip++]){
                case PUSH_IMM64:{ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)b[ip++]<<(i*8); stack.push_back((int64_t)v);} break;
                case LOAD_LOCAL:{ uint16_t idx=b[ip++]|(b[ip++]<<8); stack.push_back(locals[idx]); } break;
                case STORE_LOCAL:{ uint16_t idx=b[ip++]|(b[ip++]<<8); auto v=stack.back(); stack.pop_back(); locals[idx]=v; } break;
                case ADD:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back(ra+rb);} break;
                case MAX_:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra>rb)?ra:rb ); } break;
                case MIN_:{ auto rb=stack.back(); stack.pop_back(); auto ra=stack.back(); stack.pop_back(); stack.push_back( (ra<rb)?ra:rb ); } break;
                case RET:{ auto v=stack.back(); return (int64_t)v; }
                default: throw std::runtime_error("VM bad opcode");
            }
        }
    }
};

// ---------- NASM (PE/COFF x64) emitter
struct NASM {
    std::ostringstream asmtext;
    void prologue(int locals){
        asmtext << "default rel\n";
        asmtext << "extern ExitProcess\n";
        asmtext << "section .text\n";
        asmtext << "global main\n";
        asmtext << "main:\n";
        asmtext << "    push rbp\n";
        asmtext << "    mov rbp, rsp\n";
        // reserve locals + shadow space (32 bytes) then align
        int reserve = locals*8 + 32;
        asmtext << "    sub rsp, " << reserve << "\n";
        asmtext << "    and rsp, -16\n";
    }
    void epilogue(){
        // result is in rax, move to ecx then call ExitProcess
        asmtext << "    mov ecx, eax\n";
        asmtext << "    call ExitProcess\n"; // does not return
    }
    // virtual stack with pushes/pops in asm
    void op_push_imm(uint64_t v){
        asmtext << "    mov rax, 0x" << std::hex << v << std::dec << "\n";
        asmtext << "    push rax\n";
    }
    void op_load_local(int idx){
        int off = (idx+1)*8;
        asmtext << "    mov rax, [rbp - " << off << "]\n";
        asmtext << "    push rax\n";
    }
    void op_store_local(int idx){
        int off = (idx+1)*8;
        asmtext << "    pop rax\n";
        asmtext << "    mov [rbp - " << off << "], rax\n";
    }
    void op_add(){
        asmtext << "    pop rbx\n";
        asmtext << "    pop rax\n";
        asmtext << "    add rax, rbx\n";
        asmtext << "    push rax\n";
    }
    void op_max(){
        asmtext << "    pop rbx\n";
        asmtext << "    pop rax\n";
        asmtext << "    cmp rax, rbx\n";
        asmtext << "    cmovl rax, rbx\n"; // if a<b, take b
        asmtext << "    push rax\n";
    }
    void op_min(){
        asmtext << "    pop rbx\n";
        asmtext << "    pop rax\n";
        asmtext << "    cmp rax, rbx\n";
        asmtext << "    cmovg rax, rbx\n"; // if a>b, take b
        asmtext << "    push rax\n";
    }
    void op_ret(){
        asmtext << "    pop rax\n"; // final result in rax
        // fallthrough to epilogue
    }
};

static void emit_nasm_pe(const Code& code, int localCount, const string& outdir){
    NASM n;
    n.prologue(localCount);
    // translate IR to stack-based asm
    size_t ip=0;
    while(ip<code.bytes.size()){
        uint8_t op = code.bytes[ip++];
        switch(op){
            case PUSH_IMM64:{ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)code.bytes[ip++]<<(i*8); n.op_push_imm(v);} break;
            case LOAD_LOCAL:{ uint16_t idx=code.bytes[ip++]|(code.bytes[ip++]<<8); n.op_load_local(idx);} break;
            case STORE_LOCAL:{ uint16_t idx=code.bytes[ip++]|(code.bytes[ip++]<<8); n.op_store_local(idx);} break;
            case ADD: n.op_add(); break;
            case MAX_: n.op_max(); break;
            case MIN_: n.op_min(); break;
            case RET: n.op_ret(); goto done; // stop after RET
            default: throw std::runtime_error("NASM emitter: bad opcode");
        }
    }
done:
    n.epilogue();

    // write files
    auto ensure_dir = [&](const string& d){
        // minimal mkdir: rely on system mkdir (Windows cmd)
#ifdef _WIN32
        std::string cmd = "if not exist \""+d+"\" mkdir \""+d+"\"";
        system(cmd.c_str());
#endif
    };
    ensure_dir(outdir);
    std::string asmPath = outdir + "/parashade_main.asm";
    std::ofstream a(asmPath, std::ios::binary); a << n.asmtext.str(); a.close();

    // build.bat (nasm + link)
    std::string batPath = outdir + "/build.bat";
    std::ofstream b(batPath, std::ios::binary);
    b <<
R"(REM Build PE from NASM with MSVC LINK
@echo off
setlocal
if "%VSCMD_ARG_TGT_ARCH%"=="" (
  echo (Tip) Run from "x64 Native Tools Command Prompt for VS" so link.exe is on PATH.
)
if "%1"=="" ( set OUT=parashade.exe ) else ( set OUT=%1 )
echo Assembling...
nasm -f win64 parashade_main.asm -o parashade_main.obj || exit /b 1
echo Linking...
link /subsystem:console /entry:main parashade_main.obj kernel32.lib /out:%OUT% || exit /b 1
echo Done: %OUT%
)";
    b.close();
}

// ---------- HEX + metadata
static string hex_dump(const std::vector<uint8_t>& v){
    std::ostringstream o; o<<std::hex<<std::setfill('0');
    for(size_t i=0;i<v.size();++i){
        if(i%16==0) o<<"\n";
        o<<std::setw(2)<<(int)v[i]<<' ';
    }
    return o.str();
}

static string meta_json(const Module& m, const Typer& T, const Emitter& E){
    // locals (deterministic order)
    std::vector<const Local*> locs; locs.reserve(T.locals.size());
    for(auto& kv:T.locals) locs.push_back(&kv.second);
    std::sort(locs.begin(), locs.end(), [](auto* a, auto* b){ return a->index<b->index; });

    std::ostringstream s;
    s<<"{\n";
    s<<"  \"module\":\""<<m.name<<"\",\n";
    s<<"  \"functions\":[{\"name\":\""<<m.mainFn.name<<"\",\"locals\":[";
    for(size_t i=0;i<locs.size();++i){
        if(i) s<<",";
        s<<"{\"name\":\""<<locs[i]->name<<"\",\"type\":\"int\",\"index\":"<<locs[i]->index
         <<",\"line\":"<<locs[i]->declLine<<",\"explicit\":"<<(locs[i]->explicitInt?"true":"false")<<"}";
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

// ---------- Driver
int main(int argc, char** argv){
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    bool run=false, emit=false, emit_nasm=false;
    string nasm_outdir=".";
    for(int i=1;i<argc;i++){
        string a=argv[i];
        if(a=="--run") run=true;
        else if(a=="--emit") emit=true;
        else if(a=="--emit-nasm"){ emit_nasm=true; if(i+1<argc) nasm_outdir=argv[++i]; }
    }

    string src((std::istreambuf_iterator<char>(std::cin)), {});
    string norm = normalize_longform(src);

    try{
        Lexer L(norm); Parser P(L); Module mod=P.parseModule();
        Typer T; T.check(mod.mainFn);
        Emitter E(T); E.gen_func(mod.mainFn);

        if(run){
            VM vm(E.code.bytes, (int)T.locals.size());
            auto ret = vm.run_all();
            std::cout<< ret << "\n";
            return 0;
        }
        if(emit){
            std::cout << "; PARASHADE v0.2 HEX IR ("<< E.code.bytes.size() <<" bytes)\n";
            std::cout << hex_dump(E.code.bytes) << "\n\n; METADATA\n" << meta_json(mod,T,E);
            return 0;
        }
        if(emit_nasm){
            emit_nasm_pe(E.code, (int)T.locals.size(), nasm_outdir);
            std::cout<<"Wrote "<<nasm_outdir<<"/parashade_main.asm and build.bat\n";
            return 0;
        }
        std::cerr<<"Usage: --run | --emit | --emit-nasm <outdir>\n";
        return 1;
    } catch(const std::exception& e){
        std::cerr<<"Compile/Run error: "<<e.what()<<"\n";
        return 2;
    }
}
