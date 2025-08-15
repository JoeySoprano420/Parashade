// parashade.cpp — Parashade v0.1 seed compiler+VM (C++17, single-file)
// Build: g++ -std=cpp17 -O2 parashade.cpp -o parashade
// Usage:
//   ./parashade --run <<'PSD'
//   module Demo:
//   scope main range app:
//       declare explicit integer named x equals 0x2A end
//       declare implicit named y equals x plus 0x10 end
//       return y
//   end
//   PSD
//
//   ./parashade --emit <<'PSD' … PSD   (prints HEX IR + metadata)

#include <bits/stdc++.h>
using namespace std;

// -------- Utility
static inline string trim(string s){
    size_t a=0; while(a<s.size() && isspace((unsigned char)s[a]))++a;
    size_t b=s.size(); while(b>a && isspace((unsigned char)s[b-1]))--b;
    return s.substr(a,b-a);
}
static inline bool starts_with(const string& s, const string& p){return s.rfind(p,0)==0;}
static inline string lower(string s){ for(char& c:s) c=tolower((unsigned char)c); return s; }

// -------- Long-form ➜ Core normalizer (very small demo)
string normalize_longform(const string& in){
    // Crude phrase mapping; extend as you grow the dialect
    static const vector<pair<regex,string>> map = {
        {regex("\\bdeclare\\s+explicit\\s+integer\\s+named\\s+"), "let int "},
        {regex("\\bdeclare\\s+implicit\\s+named\\s+"), "let "},
        {regex("\\bequals\\b"), "="},
        {regex("\\bend\\b"), ""}, // end of long-form decl line; core uses EOL
        {regex("\\bplus\\b"), "+"},
        {regex("\\bgreatest_of\\b"), "max"},
        {regex("\\bleast_of\\b"), "min"},
        {regex("\\bmodule\\b"), "module"},
        {regex("\\bscope\\b"), "scope"},
        {regex("\\brange\\b"), "range"},
        {regex("\\breturn\\b"), "return"},
    };
    // Apply line by line to preserve NASM-ish feel
    string out; out.reserve(in.size());
    istringstream iss(in);
    string line;
    while(getline(iss,line)){
        string L = line;
        // NASM comments start with ';'
        auto sc = L.find(';'); if(sc!=string::npos) L = L.substr(0, sc);
        // run mappings
        for(auto& m: map) L = regex_replace(L, m.first, m.second);
        out += trim(L) + "\n";
    }
    return out;
}

// -------- Lexer (NASM-ish, whitespace separated, ':' labels, ';' comments)
enum class Tok { End, Ident, Number, Colon, Equals, Plus, KwModule, KwScope, KwRange, KwLet, KwInt, KwReturn, KwEnd };
struct Token { Tok t; string s; int line; };

struct Lexer {
    vector<Token> toks; size_t i=0;
    explicit Lexer(const string& src){
        istringstream iss(src); string line; int ln=0;
        while(getline(iss,line)){
            ++ln; string L=line;
            auto sc=L.find(';'); if(sc!=string::npos) L=L.substr(0,sc);
            istringstream ls(L); string w;
            while(ls>>w){
                if(w=="module") toks.push_back({Tok::KwModule,w,ln});
                else if(w=="scope") toks.push_back({Tok::KwScope,w,ln});
                else if(w=="range") toks.push_back({Tok::KwRange,w,ln});
                else if(w=="let") toks.push_back({Tok::KwLet,w,ln});
                else if(w=="int") toks.push_back({Tok::KwInt,w,ln});
                else if(w=="return") toks.push_back({Tok::KwReturn,w,ln});
                else if(w=="end") toks.push_back({Tok::KwEnd,w,ln});
                else if(w==":") toks.push_back({Tok::Colon,":",ln});
                else if(w=="=") toks.push_back({Tok::Equals,"=",ln});
                else if(w=="+") toks.push_back({Tok::Plus,"+",ln});
                else {
                    // number? support hex 0x...
                    if(starts_with(w,"0x") || starts_with(w,"0X")){
                        toks.push_back({Tok::Number,w,ln});
                    } else if(w.size() && w.back()==':'){
                        w.pop_back(); toks.push_back({Tok::Ident,w,ln});
                        toks.push_back({Tok::Colon,":",ln});
                    } else {
                        toks.push_back({Tok::Ident,w,ln});
                    }
                }
            }
        }
        toks.push_back({Tok::End,"",ln});
    }
    const Token& peek() const { return toks[i]; }
    Token pop(){ return toks[i++]; }
    bool accept(Tok t){ if(peek().t==t){ ++i; return true; } return false; }
    void expect(Tok t, const char* msg){ if(!accept(t)) throw runtime_error(string("Parse error: expected ")+msg+" at line "+to_string(peek().line)); }
};

// -------- AST (tiny)
struct Expr {
    enum Kind { Num, Var, Add } kind;
    uint64_t val=0; string name; unique_ptr<Expr> a,b;
    static Expr num(uint64_t v){ Expr e; e.kind=Num; e.val=v; return e; }
    static Expr var(string n){ Expr e; e.kind=Var; e.name=move(n); return e; }
    static unique_ptr<Expr> add(unique_ptr<Expr> A, unique_ptr<Expr> B){
        auto p=make_unique<Expr>(); p->kind=Add; p->a=move(A); p->b=move(B); return p;
    }
};

struct Stmt {
    enum Kind { Let, Ret } kind;
    string name; bool isInt=false;
    unique_ptr<Expr> expr;
    static Stmt makeLet(string n,bool isInt, unique_ptr<Expr> e){ Stmt s; s.kind=Let;s.name=move(n);s.isInt=isInt;s.expr=move(e); return s; }
    static Stmt makeRet(unique_ptr<Expr> e){ Stmt s; s.kind=Ret; s.expr=move(e); return s; }
};

struct Func {
    string name; vector<Stmt> body;
};

struct Module {
    string name; Func mainFn;
};

// -------- Parser
struct Parser {
    Lexer& L;
    explicit Parser(Lexer& l):L(l){}
    Module parseModule(){
        L.expect(Tok::KwModule,"module"); auto id=L.pop();
        if(id.t!=Tok::Ident) throw runtime_error("module: expected identifier");
        L.expect(Tok::Colon,":");
        Module m; m.name=id.s;
        m.mainFn = parseScope(); // v0.1 expects a single scope main
        return m;
    }
    Func parseScope(){
        L.expect(Tok::KwScope,"scope");
        auto id=L.pop(); if(id.t!=Tok::Ident || id.s!="main") throw runtime_error("only 'scope main' supported in v0.1");
        L.expect(Tok::KwRange,"range");
        auto r=L.pop(); if(r.t!=Tok::Ident) throw runtime_error("range: expected identifier");
        L.expect(Tok::Colon,":");
        Func f; f.name="main";
        // statements until 'end'
        while(L.peek().t!=Tok::KwEnd && L.peek().t!=Tok::End){
            f.body.push_back(parseStmt());
        }
        L.expect(Tok::KwEnd,"end");
        return f;
    }
    Stmt parseStmt(){
        if(L.peek().t==Tok::KwLet){
            L.pop();
            bool isInt=false;
            if(L.accept(Tok::KwInt)) isInt=true;
            auto id=L.pop(); if(id.t!=Tok::Ident) throw runtime_error("let: expected name");
            L.expect(Tok::Equals,"=");
            auto e=parseExpr();
            return Stmt::makeLet(id.s,isInt,move(e));
        } else if(L.peek().t==Tok::KwReturn){
            L.pop(); auto e=parseExpr(); return Stmt::makeRet(move(e));
        }
        throw runtime_error("Unknown statement at line "+to_string(L.peek().line));
    }
    unique_ptr<Expr> parseExpr(){
        // expr := term ('+' term)*
        auto t=parseTerm();
        while(L.accept(Tok::Plus)){
            auto r=parseTerm();
            t=Expr::add(move(t), move(r));
        }
        return t;
    }
    unique_ptr<Expr> parseTerm(){
        auto tk=L.pop();
        if(tk.t==Tok::Number){
            uint64_t v=0; std::stringstream ss; ss<<std::hex<<tk.s.substr(2); ss>>v;
            return make_unique<Expr>(Expr::num(v));
        } else if(tk.t==Tok::Ident){
            auto e=make_unique<Expr>(Expr::var(tk.s)); return e;
        }
        throw runtime_error("Expected number or ident at line "+to_string(tk.line));
    }
};

// -------- Static types, locals table
struct Type { enum K { Int } k; };
struct Local { string name; Type ty; int index; };

struct Typer {
    unordered_map<string,Local> locals;
    int nextIdx=0;
    void ensure_local(const string& n, bool declaredInt){
        if(locals.count(n)) return;
        locals[n] = Local{n, Type{Type::Int}, nextIdx++};
    }
    void check(const Func& f){
        for(auto& s: f.body){
            if(s.kind==Stmt::Let){
                ensure_local(s.name, s.isInt);
                // v0.1: only ints; expression must be int
                // (we would walk `s.expr` and ensure all vars resolved)
            } else if(s.kind==Stmt::Ret){
                // ensure expr is int; omitted for brevity
            }
        }
    }
    int localIndex(const string& n) const {
        auto it=locals.find(n); if(it==locals.end()) throw runtime_error("use of undeclared "+n);
        return it->second.index;
    }
};

// -------- IR emitter (hex bytecode)
enum Op: uint8_t { PUSH_IMM64=0x01, ADD=0x02, SUB=0x03, MUL=0x04, DIV_=0x05, STORE_LOCAL=0x10, LOAD_LOCAL=0x11, RET=0x21 };
struct Code { vector<uint8_t> bytes; };

void emit_u8(Code& c, uint8_t v){ c.bytes.push_back(v); }
void emit_u64(Code& c, uint64_t v){ for(int i=0;i<8;++i) emit_u8(c, (v>>(i*8))&0xFF); }
void emit_u16(Code& c, uint16_t v){ emit_u8(c, v&0xFF); emit_u8(c, (v>>8)&0xFF); }

struct Emitter {
    Code code;
    Typer& T;
    Emitter(Typer& t):T(t){}
    void gen_expr(const Expr* e){
        switch(e->kind){
            case Expr::Num: emit_u8(code,PUSH_IMM64); emit_u64(code,e->val); break;
            case Expr::Var: emit_u8(code,LOAD_LOCAL); emit_u16(code, (uint16_t)T.localIndex(e->name)); break;
            case Expr::Add: gen_expr(e->a.get()); gen_expr(e->b.get()); emit_u8(code,ADD); break;
        }
    }
    void gen_func(const Func& f){
        for(auto& s: f.body){
            if(s.kind==Stmt::Let){
                gen_expr(s.expr.get());
                emit_u8(code, STORE_LOCAL);
                emit_u16(code, (uint16_t)T.localIndex(s.name));
            } else if(s.kind==Stmt::Ret){
                gen_expr(s.expr.get());
                emit_u8(code, RET);
            }
        }
    }
};

// -------- Capsules (demo)
struct CapsuleArena {
    vector<uint8_t> buf; size_t off=0;
    explicit CapsuleArena(size_t cap=1<<20){ buf.resize(cap); }
    void* alloc(size_t n){ if(off+n>buf.size()) throw bad_alloc(); void* p=&buf[off]; off+=n; return p; }
    void reset(){ off=0; } // range end
};

// -------- Error containers (very small)
struct ErrorContainer {
    string code, explain, fallback;
};

// -------- VM + frame interpreter
struct VM {
    const vector<uint8_t>& b; vector<int64_t> stack; vector<int64_t> locals;
    explicit VM(const vector<uint8_t>& bytes, int localCount):b(bytes), locals(localCount,0){}
    int64_t run_all(){
        size_t ip=0; for(;;){
            if(ip>=b.size()) throw runtime_error("VM: OOB");
            switch(b[ip++]){
                case PUSH_IMM64:{ uint64_t v=0; for(int i=0;i<8;++i) v |= (uint64_t)b[ip++]<<(i*8); stack.push_back((int64_t)v); } break;
                case LOAD_LOCAL:{ uint16_t idx = b[ip++] | (b[ip++]<<8); stack.push_back(locals[idx]); } break;
                case STORE_LOCAL:{ uint16_t idx = b[ip++] | (b[ip++]<<8); auto v=stack.back(); stack.pop_back(); locals[idx]=v; } break;
                case ADD:{ auto b2=stack.back(); stack.pop_back(); auto a=stack.back(); stack.pop_back(); stack.push_back(a+b2);} break;
                case RET:{ auto v=stack.back(); return v; }
                default: throw runtime_error("VM: bad opcode");
            }
        }
    }
    // frame-by-frame interpreter (budgeted steps)
    bool run_frame(int maxSteps, int64_t& last){
        static size_t ip=0; // demo: static to simulate persistent frame progress
        int steps=0;
        while(steps<maxSteps && ip<b.size()){
            switch(b[ip++]){
                case PUSH_IMM64:{ uint64_t v=0; for(int i=0;i<8;++i) v |= (uint64_t)b[ip++]<<(i*8); stack.push_back((int64_t)v);} break;
                case LOAD_LOCAL:{ uint16_t idx = b[ip++] | (b[ip++]<<8); stack.push_back(locals[idx]); } break;
                case STORE_LOCAL:{ uint16_t idx = b[ip++] | (b[ip++]<<8); auto v=stack.back(); stack.pop_back(); locals[idx]=v; } break;
                case ADD:{ auto b2=stack.back(); stack.pop_back(); auto a=stack.back(); stack.pop_back(); stack.push_back(a+b2);} break;
                case RET:{ last = stack.back(); stack.pop_back(); ip=0; return true; }
                default: throw runtime_error("VM: bad opcode");
            }
            ++steps;
        }
        return false; // not finished this frame
    }
};

// -------- Metadata emission (tiny JSON without deps)
string meta_json(const Module& m, const Typer& T){
    // Very small metadata: module, function, locals count
    string s="{\"module\":\""+m.name+"\",\"functions\":[{\"name\":\""+m.mainFn.name+"\",\"locals\":[";
    bool first=true; for(auto& kv: T.locals){ if(!first) s+=','; first=false; s+="{\"name\":\""+kv.first+"\",\"type\":\"int\"}"; }
    s+="]}]}"; return s;
}

// -------- Driver
int main(int argc, char** argv){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    bool run = (argc>1 && string(argv[1])=="--run");
    bool emit = (argc>1 && string(argv[1])=="--emit");
    string src((istreambuf_iterator<char>(cin)), {});
    string norm = normalize_longform(src);
    try{
        Lexer lex(norm);
        Parser p(lex);
        Module mod = p.parseModule();
        Typer typer; typer.check(mod.mainFn);

        Emitter em(typer); em.gen_func(mod.mainFn);

        if(run){
            VM vm(em.code.bytes, (int)typer.locals.size());
            // Choose mode: AOT or frame-by-frame JIT
            bool useFrameJit=false;
            if(norm.find("swear_by_frame_jit")!=string::npos) useFrameJit=true;

            if(useFrameJit){
                int64_t result=0;
                for(int frame=0; frame<8; ++frame){
                    bool done = vm.run_frame(/*max steps*/16, result);
                    if(done){ cout<<result<<"\n"; return 0; }
                }
                // if not done in N frames, fall back to AOT run_all
                cout<<vm.run_all()<<"\n";
            } else {
                cout<<vm.run_all()<<"\n";
            }
            return 0;
        } else if(emit){
            // HEX dump
            cout << "; PARASHADE v0.1 HEX IR ("<< em.code.bytes.size() <<" bytes)\n";
            cout << hex << setfill('0');
            for(size_t i=0;i<em.code.bytes.size();++i){
                if(i%16==0) cout<<"\n";
                cout<<setw(2)<<(int)em.code.bytes[i]<<' ';
            }
            cout<<dec<<"\n\n; METADATA\n"<< meta_json(mod, typer) <<"\n";
            return 0;
        } else {
            cerr<<"Usage: --run | --emit (reads source from stdin)\n";
            return 1;
        }
    } catch(const exception& e){
        cerr<<"Compile/Run error: "<<e.what()<<"\n";
        return 2;
    }
}
