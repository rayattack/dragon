// D052 `dragon ffi sync`: stubs are always regenerated (DO NOT EDIT); skeletons
// are written once and never overwritten; --check diffs stubs and writes nothing.
#include "FfiSync.h"
#include "dragon/AST.h"
#include "dragon/Lexer.h"
#include "dragon/Parser.h"

#include "FfiSync.h"
#include "dragon/AST.h"
#include "dragon/Lexer.h"
#include "dragon/Parser.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>


namespace dragon {
namespace {
namespace fs = std::filesystem;

struct ClassField { std::string name; const TypeExpr* type; };
using ClassMap = std::map<std::string, std::vector<ClassField>>;

// The field/param shapes the process lane carries (mirrors the wrapper + encode[T]).
struct FKind {
    enum Base { Scalar, Klass, ListScalar, ListClass, DictScalar, OptScalar, OptClass, Blob, Bad };
    Base base = Bad;
    std::string name;  // scalar spelling (int/str/bool/float) or the class name
};

bool isScalarName(const std::string& n) {
    return n == "int" || n == "str" || n == "bool" || n == "float";
}

FKind parseKind(const TypeExpr* t, const ClassMap& classes) {
    FKind k;
    if (auto* nt = dynamic_cast<const NamedTypeExpr*>(t)) {
        if (isScalarName(nt->name)) { k.base = FKind::Scalar; k.name = nt->name; }
        else if (nt->name == "bytes") { k.base = FKind::Blob; k.name = "bytes"; }
        else if (classes.count(nt->name)) { k.base = FKind::Klass; k.name = nt->name; }
        return k;
    }
    if (auto* gt = dynamic_cast<const GenericTypeExpr*>(t)) {
        auto* b = dynamic_cast<const NamedTypeExpr*>(gt->base.get());
        if (b && b->name == "list" && gt->typeArgs.size() == 1) {
            auto* el = dynamic_cast<const NamedTypeExpr*>(gt->typeArgs[0].get());
            if (!el) return k;
            if (isScalarName(el->name)) { k.base = FKind::ListScalar; k.name = el->name; }
            else if (classes.count(el->name)) { k.base = FKind::ListClass; k.name = el->name; }
            return k;
        }
        if (b && b->name == "dict" && gt->typeArgs.size() == 2) {
            auto* kt = dynamic_cast<const NamedTypeExpr*>(gt->typeArgs[0].get());
            auto* vt = dynamic_cast<const NamedTypeExpr*>(gt->typeArgs[1].get());
            if (kt && vt && kt->name == "str" && isScalarName(vt->name)) {
                k.base = FKind::DictScalar;
                k.name = vt->name;
            }
            return k;
        }
        return k;
    }
    if (auto* ut = dynamic_cast<const UnionTypeExpr*>(t)) {
        if (ut->types.size() != 2) return k;
        const TypeExpr* inner = nullptr;
        bool hasNone = false;
        for (auto& tt : ut->types) {
            auto* nt = dynamic_cast<const NamedTypeExpr*>(tt.get());
            if (nt && nt->name == "None") hasNone = true;
            else inner = tt.get();
        }
        if (!hasNone || !inner) return k;
        FKind ik = parseKind(inner, classes);
        if (ik.base == FKind::Scalar) { k.base = FKind::OptScalar; k.name = ik.name; }
        else if (ik.base == FKind::Klass) { k.base = FKind::OptClass; k.name = ik.name; }
        return k;
    }
    return k;
}

// Dragon spelling of a TypeExpr for the embedded signature line.
std::string dragonTypeName(const TypeExpr* t) {
    if (auto* nt = dynamic_cast<const NamedTypeExpr*>(t)) return nt->name;
    if (auto* gt = dynamic_cast<const GenericTypeExpr*>(t)) {
        std::string s = dragonTypeName(gt->base.get()) + "[";
        for (size_t i = 0; i < gt->typeArgs.size(); ++i) {
            if (i) s += ", ";
            s += dragonTypeName(gt->typeArgs[i].get());
        }
        return s + "]";
    }
    if (auto* ut = dynamic_cast<const UnionTypeExpr*>(t)) {
        std::string s;
        for (size_t i = 0; i < ut->types.size(); ++i) {
            if (i) s += " | ";
            s += dragonTypeName(ut->types[i].get());
        }
        return s;
    }
    return "?";
}

std::string signatureOf(const FunctionDecl* fn) {
    std::string s = fn->name + "(";
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) s += ", ";
        s += fn->params[i].name + ": " + dragonTypeName(fn->params[i].type.get());
    }
    return s + ") -> " + dragonTypeName(fn->returnType.get());
}

// Classes an extern reaches, dependencies first (nested classes precede users).
std::vector<std::string> referencedClasses(const FunctionDecl* fn, const ClassMap& classes,
                                           std::string& missing) {
    std::vector<std::string> order;
    std::set<std::string> seen;
    std::function<void(const TypeExpr*)> visitType = [&](const TypeExpr* t) {
        FKind k = parseKind(t, classes);
        if (k.base == FKind::Bad || k.base == FKind::Blob ||
            isScalarName(k.name) || k.name.empty()) return;
        if (seen.count(k.name)) return;
        seen.insert(k.name);
        auto it = classes.find(k.name);
        if (it == classes.end()) { if (missing.empty()) missing = k.name; return; }
        for (auto& f : it->second) visitType(f.type);
        order.push_back(k.name);
    };
    for (auto& p : fn->params) visitType(p.type.get());
    visitType(fn->returnType.get());
    return order;
}

std::string capitalize(std::string s) {
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

// ---- Python ----

std::string pyTypeOf(const FKind& k) {
    switch (k.base) {
        case FKind::Scalar: return k.name;
        case FKind::Klass: return k.name;
        case FKind::ListScalar: return "list[" + k.name + "]";
        case FKind::ListClass: return "list[" + k.name + "]";
        case FKind::DictScalar: return "dict[str, " + k.name + "]";
        case FKind::OptScalar: return k.name + " | None";
        case FKind::OptClass: return k.name + " | None";
        case FKind::Blob: return "bytes";
        default: return "object";
    }
}

// Expression converting the raw json value `src` into the typed shape.
std::string pyInExpr(const FKind& k, const std::string& src) {
    switch (k.base) {
        case FKind::Klass: return "_in_" + k.name + "(" + src + ")";
        case FKind::ListClass: return "[_in_" + k.name + "(e) for e in " + src + "]";
        case FKind::OptClass: return "(None if " + src + " is None else _in_" + k.name + "(" + src + "))";
        default: return src;
    }
}

std::string renderPythonStub(const FunctionDecl* fn, const std::vector<std::string>& order,
                             const ClassMap& classes, const std::string& srcBase) {
    std::ostringstream o;
    o << "# AUTO-GENERATED by `dragon ffi sync` from " << srcBase
      << " - DO NOT EDIT (always regenerated).\n";
    o << "# signature: " << signatureOf(fn) << "\n";
    o << "import json\nimport struct\nimport sys\nimport traceback\n"
      << "from dataclasses import dataclass, asdict\n";
    for (auto& cname : order) {
        o << "\n\n@dataclass\nclass " << cname << ":\n";
        for (auto& f : classes.at(cname))
            o << "    " << f.name << ": " << pyTypeOf(parseKind(f.type, classes)) << "\n";
    }
    for (auto& cname : order) {
        o << "\n\ndef _in_" << cname << "(v):\n    return " << cname << "(";
        const auto& fields = classes.at(cname);
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) o << ", ";
            FKind k = parseKind(fields[i].type, classes);
            const std::string src = (k.base == FKind::OptScalar || k.base == FKind::OptClass)
                ? "v.get(\"" + fields[i].name + "\")"
                : "v[\"" + fields[i].name + "\"]";
            o << fields[i].name << "=" << pyInExpr(k, src);
        }
        o << ")\n";
    }
    o << "\n\ndef _out(v):\n"
      << "    if isinstance(v, list):\n        return [_out(x) for x in v]\n"
      << "    if hasattr(v, \"__dataclass_fields__\"):\n        return asdict(v)\n"
      << "    return v\n";
    o << "\n\ndef _reply(header, payload=b\"\"):\n"
      << "    sys.stdout.buffer.write(struct.pack(\"<I\", len(header)) + header + payload)\n"
      << "    sys.stdout.buffer.flush()\n";
    // The baton frame loop: EOF on the length prefix is a clean shutdown; a
    // raised exception is an ok:false frame and the child keeps serving.
    o << "\n\ndef serve(fn):\n"
      << "    while True:\n"
      << "        pre = sys.stdin.buffer.read(4)\n"
      << "        if len(pre) < 4:\n"
      << "            return\n"
      << "        hlen = struct.unpack(\"<I\", pre)[0]\n"
      << "        header = json.loads(sys.stdin.buffer.read(hlen))\n"
      << "        body = sys.stdin.buffer.read(header.get(\"body_len\", 0))\n"
      << "        blobs = [sys.stdin.buffer.read(n) for n in header.get(\"blobs\", [])]\n"
      << "        payload = json.loads(body) if body else {}\n"
      << "        try:\n"
      << "            result = fn(";
    size_t blobIdx = 0;
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) o << ", ";
        FKind k = parseKind(fn->params[i].type.get(), classes);
        if (k.base == FKind::Blob)
            o << fn->params[i].name << "=blobs[" << blobIdx++ << "]";
        else
            o << fn->params[i].name << "="
              << pyInExpr(k, "payload[\"" + fn->params[i].name + "\"]");
    }
    o << ")\n"
      << "        except Exception:\n"
      << "            err = traceback.format_exc().encode()\n"
      << "            _reply(json.dumps({\"ok\": False, \"error_len\": len(err)}).encode(), err)\n"
      << "            continue\n"
      << "        out = _out(result)\n"
      << "        if isinstance(out, (bytes, bytearray)):\n"
      << "            body_out = json.dumps({\"$blob\": 0}).encode()\n"
      << "            rh = json.dumps({\"ok\": True, \"body_len\": len(body_out), \"blobs\": [len(out)]}).encode()\n"
      << "            _reply(rh, body_out + bytes(out))\n"
      << "        else:\n"
      << "            body_out = json.dumps(out).encode()\n"
      << "            rh = json.dumps({\"ok\": True, \"body_len\": len(body_out), \"blobs\": []}).encode()\n"
      << "            _reply(rh, body_out)\n";
    return o.str();
}

std::string renderPythonSkeleton(const FunctionDecl* fn, const std::vector<std::string>& order,
                                 const std::string& stubModule) {
    std::ostringstream o;
    o << "# Written once by `dragon ffi sync` - this file is yours; edit freely.\n";
    o << "from " << stubModule << " import ";
    for (auto& c : order) o << c << ", ";
    o << "serve\n\n\ndef " << fn->name << "(";
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) o << ", ";
        o << fn->params[i].name;
    }
    o << "):\n    raise NotImplementedError(\"" << signatureOf(fn) << "\")\n\n\n"
      << "serve(" << fn->name << ")\n";
    return o.str();
}

// ---- Go ----

std::string goTypeOf(const FKind& k) {
    auto scalar = [](const std::string& n) -> std::string {
        if (n == "int") return "int64";
        if (n == "float") return "float64";
        if (n == "bool") return "bool";
        return "string";
    };
    switch (k.base) {
        case FKind::Scalar: return scalar(k.name);
        case FKind::Klass: return k.name;
        case FKind::ListScalar: return "[]" + scalar(k.name);
        case FKind::ListClass: return "[]" + k.name;
        case FKind::DictScalar: return "map[string]" + scalar(k.name);
        case FKind::OptScalar: return "*" + scalar(k.name);
        case FKind::OptClass: return "*" + k.name;
        case FKind::Blob: return "[]byte";
        default: return "any";
    }
}

std::string renderGoStub(const FunctionDecl* fn, const std::vector<std::string>& order,
                         const ClassMap& classes, const std::string& srcBase,
                         const std::string& binBase) {
    const bool bytesRet = parseKind(fn->returnType.get(), classes).base == FKind::Blob;
    std::ostringstream o;
    o << "// AUTO-GENERATED by `dragon ffi sync` from " << srcBase
      << " - DO NOT EDIT (always regenerated).\n";
    o << "// signature: " << signatureOf(fn) << " | build: go build -o " << binBase
      << " " << binBase << ".go " << binBase << "_stub.go\n";
    o << "package main\n\nimport (\n\t\"bufio\"\n\t\"encoding/binary\"\n"
      << "\t\"encoding/json\"\n\t\"fmt\"\n\t\"io\"\n\t\"os\"\n)\n";
    for (auto& cname : order) {
        o << "\ntype " << cname << " struct {\n";
        for (auto& f : classes.at(cname))
            o << "\t" << capitalize(f.name) << " " << goTypeOf(parseKind(f.type, classes))
              << " `json:\"" << f.name << "\"`\n";
        o << "}\n";
    }
    o << "\nfunc dragonReply(out *bufio.Writer, header []byte, payload []byte) {\n"
      << "\tvar pre [4]byte\n"
      << "\tbinary.LittleEndian.PutUint32(pre[:], uint32(len(header)))\n"
      << "\tout.Write(pre[:])\n\tout.Write(header)\n\tout.Write(payload)\n\tout.Flush()\n}\n";
    // The baton frame loop: EOF on the length prefix is a clean shutdown.
    o << "\nfunc main() {\n"
      << "\tin := bufio.NewReader(os.Stdin)\n"
      << "\tout := bufio.NewWriter(os.Stdout)\n"
      << "\tfor {\n"
      << "\t\tvar pre [4]byte\n"
      << "\t\tif _, err := io.ReadFull(in, pre[:]); err != nil {\n\t\t\treturn\n\t\t}\n"
      << "\t\thlen := binary.LittleEndian.Uint32(pre[:])\n"
      << "\t\thb := make([]byte, hlen)\n"
      << "\t\tio.ReadFull(in, hb)\n"
      << "\t\tvar h struct {\n"
      << "\t\t\tBodyLen int   `json:\"body_len\"`\n"
      << "\t\t\tBlobs   []int `json:\"blobs\"`\n"
      << "\t\t}\n"
      << "\t\tjson.Unmarshal(hb, &h)\n"
      << "\t\tbody := make([]byte, h.BodyLen)\n"
      << "\t\tio.ReadFull(in, body)\n"
      << "\t\tblobs := make([][]byte, len(h.Blobs))\n"
      << "\t\tfor i, n := range h.Blobs {\n"
      << "\t\t\tblobs[i] = make([]byte, n)\n\t\t\tio.ReadFull(in, blobs[i])\n\t\t}\n"
      << "\t\t_ = blobs\n"
      << "\t\tvar args struct {\n";
    for (auto& p : fn->params) {
        FKind k = parseKind(p.type.get(), classes);
        if (k.base == FKind::Blob) continue;  // rides as a blob, not in the body
        o << "\t\t\t" << capitalize(p.name) << " " << goTypeOf(k)
          << " `json:\"" << p.name << "\"`\n";
    }
    o << "\t\t}\n"
      << "\t\tjson.Unmarshal(body, &args)\n"
      << "\t\tresult := " << fn->name << "(";
    size_t goBlobIdx = 0;
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) o << ", ";
        FKind k = parseKind(fn->params[i].type.get(), classes);
        if (k.base == FKind::Blob) o << "blobs[" << goBlobIdx++ << "]";
        else o << "args." << capitalize(fn->params[i].name);
    }
    o << ")\n";
    if (bytesRet) {
        o << "\t\trh := []byte(fmt.Sprintf(`{\"ok\":true,\"body_len\":11,\"blobs\":[%d]}`, len(result)))\n"
          << "\t\tdragonReply(out, rh, append([]byte(`{\"$blob\":0}`), result...))\n";
    } else {
        o << "\t\tob, _ := json.Marshal(result)\n"
          << "\t\trh := []byte(fmt.Sprintf(`{\"ok\":true,\"body_len\":%d,\"blobs\":[]}`, len(ob)))\n"
          << "\t\tdragonReply(out, rh, ob)\n";
    }
    o << "\t}\n}\n";
    return o.str();
}

std::string renderGoSkeleton(const FunctionDecl* fn, const ClassMap& classes) {
    std::ostringstream o;
    o << "// Written once by `dragon ffi sync` - this file is yours; edit freely.\n"
      << "package main\n\nfunc " << fn->name << "(";
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) o << ", ";
        o << fn->params[i].name << " " << goTypeOf(parseKind(fn->params[i].type.get(), classes));
    }
    o << ") " << goTypeOf(parseKind(fn->returnType.get(), classes)) << " {\n"
      << "\tpanic(\"implement " << signatureOf(fn) << "\")\n}\n";
    return o.str();
}

// ---- Rust ----

std::string rustTypeOf(const FKind& k) {
    auto scalar = [](const std::string& n) -> std::string {
        if (n == "int") return "i64";
        if (n == "float") return "f64";
        if (n == "bool") return "bool";
        return "String";
    };
    switch (k.base) {
        case FKind::Scalar: return scalar(k.name);
        case FKind::Klass: return k.name;
        case FKind::ListScalar: return "Vec<" + scalar(k.name) + ">";
        case FKind::ListClass: return "Vec<" + k.name + ">";
        case FKind::DictScalar: return "std::collections::HashMap<String, " + scalar(k.name) + ">";
        case FKind::OptScalar: return "Option<" + scalar(k.name) + ">";
        case FKind::OptClass: return "Option<" + k.name + ">";
        case FKind::Blob: return "Vec<u8>";
        default: return "serde_json::Value";
    }
}

std::string renderRustStub(const FunctionDecl* fn, const std::vector<std::string>& order,
                           const ClassMap& classes, const std::string& srcBase,
                           const std::string& binBase) {
    const bool bytesRet = parseKind(fn->returnType.get(), classes).base == FKind::Blob;
    std::ostringstream o;
    o << "// AUTO-GENERATED by `dragon ffi sync` from " << srcBase
      << " - DO NOT EDIT (always regenerated).\n";
    o << "// signature: " << signatureOf(fn)
      << " | build: needs serde + serde_json (cargo add serde --features derive serde_json)\n";
    o << "use serde::{Deserialize, Serialize};\nuse std::io::{Read, Write};\n\n"
      << "include!(\"" << binBase << ".rs\");\n";
    for (auto& cname : order) {
        o << "\n#[derive(Serialize, Deserialize)]\npub struct " << cname << " {\n";
        for (auto& f : classes.at(cname))
            o << "    pub " << f.name << ": " << rustTypeOf(parseKind(f.type, classes)) << ",\n";
        o << "}\n";
    }
    o << "\n#[derive(Deserialize)]\nstruct DragonHeader {\n"
      << "    body_len: Option<usize>,\n    blobs: Option<Vec<usize>>,\n}\n";
    o << "\n#[derive(Deserialize)]\nstruct DragonArgs {\n";
    for (auto& p : fn->params) {
        FKind k = parseKind(p.type.get(), classes);
        if (k.base == FKind::Blob) continue;  // rides as a blob, not in the body
        o << "    " << p.name << ": " << rustTypeOf(k) << ",\n";
    }
    o << "}\n";
    // The baton frame loop: EOF on the length prefix is a clean shutdown.
    o << "\nfn main() {\n"
      << "    let mut sin = std::io::stdin().lock();\n"
      << "    let mut sout = std::io::stdout().lock();\n"
      << "    loop {\n"
      << "        let mut pre = [0u8; 4];\n"
      << "        if sin.read_exact(&mut pre).is_err() {\n            return;\n        }\n"
      << "        let hlen = u32::from_le_bytes(pre) as usize;\n"
      << "        let mut hb = vec![0u8; hlen];\n"
      << "        sin.read_exact(&mut hb).expect(\"header\");\n"
      << "        let h: DragonHeader = serde_json::from_slice(&hb).expect(\"header json\");\n"
      << "        let mut body = vec![0u8; h.body_len.unwrap_or(0)];\n"
      << "        sin.read_exact(&mut body).expect(\"body\");\n"
      << "        let mut blobs: Vec<Vec<u8>> = Vec::new();\n"
      << "        for n in h.blobs.unwrap_or_default() {\n"
      << "            let mut b = vec![0u8; n];\n"
      << "            sin.read_exact(&mut b).expect(\"blob\");\n"
      << "            blobs.push(b);\n        }\n"
      << "        let _ = &blobs;\n"
      << "        let a: DragonArgs = serde_json::from_slice(&body).expect(\"args json\");\n"
      << "        let result = " << fn->name << "(";
    size_t rsBlobIdx = 0;
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) o << ", ";
        FKind k = parseKind(fn->params[i].type.get(), classes);
        if (k.base == FKind::Blob) o << "blobs[" << rsBlobIdx++ << "].clone()";
        else o << "a." << fn->params[i].name;
    }
    o << ");\n";
    if (bytesRet) {
        o << "        let body_out = b\"{\\\"$blob\\\":0}\".to_vec();\n"
          << "        let rh = format!(\"{{\\\"ok\\\":true,\\\"body_len\\\":{},\\\"blobs\\\":[{}]}}\", body_out.len(), result.len());\n"
          << "        sout.write_all(&(rh.len() as u32).to_le_bytes()).unwrap();\n"
          << "        sout.write_all(rh.as_bytes()).unwrap();\n"
          << "        sout.write_all(&body_out).unwrap();\n"
          << "        sout.write_all(&result).unwrap();\n"
          << "        sout.flush().unwrap();\n";
    } else {
        o << "        let ob = serde_json::to_vec(&result).expect(\"encode\");\n"
          << "        let rh = format!(\"{{\\\"ok\\\":true,\\\"body_len\\\":{},\\\"blobs\\\":[]}}\", ob.len());\n"
          << "        sout.write_all(&(rh.len() as u32).to_le_bytes()).unwrap();\n"
          << "        sout.write_all(rh.as_bytes()).unwrap();\n"
          << "        sout.write_all(&ob).unwrap();\n"
          << "        sout.flush().unwrap();\n";
    }
    o << "    }\n}\n";
    return o.str();
}

std::string renderRustSkeleton(const FunctionDecl* fn, const ClassMap& classes) {
    std::ostringstream o;
    o << "// Written once by `dragon ffi sync` - this file is yours; edit freely.\n\n"
      << "fn " << fn->name << "(";
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i) o << ", ";
        o << fn->params[i].name << ": " << rustTypeOf(parseKind(fn->params[i].type.get(), classes));
    }
    o << ") -> " << rustTypeOf(parseKind(fn->returnType.get(), classes)) << " {\n"
      << "    todo!(\"" << signatureOf(fn) << "\")\n}\n";
    return o.str();
}

// Provenance from an existing stub's AUTO-GENERATED line: the owning .dr file,
// or "" when the marker is absent (not one of ours - never overwrite it).
std::string stubOwner(const std::string& content) {
    const std::string tag = "AUTO-GENERATED by `dragon ffi sync` from ";
    auto at = content.find(tag);
    if (at == std::string::npos) return "";
    auto start = at + tag.size();
    auto end = content.find(" - DO NOT EDIT", start);
    if (end == std::string::npos) return "";
    return content.substr(start, end - start);
}

// Where an extern's generated stub lives (shared by sync and dragon-check).
fs::path stubPathFor(const FunctionDecl* fn, const fs::path& srcDir) {
    fs::path target(fn->externPath);
    if (target.is_relative()) target = srcDir / target;
    if (fn->externLang == "python")
        return target.parent_path() / (target.stem().string() + "_stub.py");
    const std::string ext = fn->externLang == "golang" ? ".go" : ".rs";
    return target.parent_path() / (target.filename().string() + "_stub" + ext);
}

std::string readWhole(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeWhole(const fs::path& p, const std::string& content) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return true;
}

}  // namespace

int runFfiSync(const std::string& filename, bool checkOnly) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) {
        std::cerr << "dragon ffi sync: cannot read " << filename << "\n";
        return 1;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    // Token lexemes are views into the source: it must outlive lex AND parse.
    const std::string source = ss.str();

    LexerOptions lexOpts;
    lexOpts.useBraceBlocks = true;
    lexOpts.filename = filename;
    Lexer lexer(source, lexOpts);
    auto tokens = lexer.tokenize();
    if (lexer.hasErrors()) {
        std::cerr << "dragon ffi sync: " << filename << " has lex errors; fix them first\n";
        return 1;
    }
    ParserOptions parseOpts;
    parseOpts.isDragonFile = true;
    parseOpts.filename = filename;
    Parser parser(std::move(tokens), parseOpts);
    auto module = parser.parseModule();
    if (parser.hasErrors() || !module) {
        std::cerr << "dragon ffi sync: " << filename << " has parse errors; fix them first\n";
        return 1;
    }

    // Same-file classes (v1 scope): name -> ctor fields.
    ClassMap classes;
    for (auto& stmt : module->body) {
        auto* cd = dynamic_cast<ClassDecl*>(stmt.get());
        if (!cd) continue;
        for (auto& m : cd->body) {
            auto* fd = dynamic_cast<FunctionDecl*>(m.get());
            if (!fd || !(fd->isConstructor || fd->name == "__init__")) continue;
            auto& fields = classes[cd->name];
            for (auto& p : fd->params)
                if (p.name != "self") fields.push_back({p.name, p.type.get()});
            break;
        }
    }

    const std::string srcBase = fs::path(filename).filename().string();
    const fs::path srcDir = fs::path(filename).parent_path();
    std::map<std::string, std::string> usedPaths;  // from-path -> extern name
    std::vector<std::string> stale;

    for (auto& stmt : module->body) {
        auto* fn = dynamic_cast<FunctionDecl*>(stmt.get());
        if (!fn || fn->externLang.empty()) continue;

        auto prev = usedPaths.find(fn->externPath);
        if (prev != usedPaths.end()) {
            std::cerr << "dragon ffi sync: externs '" << prev->second << "' and '" << fn->name
                      << "' both point at \"" << fn->externPath << "\" - one script/binary per extern\n";
            return 1;
        }
        usedPaths[fn->externPath] = fn->name;

        std::string missing;
        auto order = referencedClasses(fn, classes, missing);
        if (!missing.empty()) {
            std::cerr << "dragon ffi sync: extern '" << fn->name << "' references class '"
                      << missing << "', which is not declared in " << srcBase
                      << " (v1 syncs same-file classes only)\n";
            return 1;
        }

        fs::path target(fn->externPath);
        if (target.is_relative()) target = srcDir / target;
        fs::path stubPath = stubPathFor(fn, srcDir), skelPath;
        std::string stubContent, skelContent;
        if (fn->externLang == "python") {
            const std::string base = target.stem().string();
            skelPath = target;
            stubContent = renderPythonStub(fn, order, classes, srcBase);
            skelContent = renderPythonSkeleton(fn, order, base + "_stub");
        } else {
            const std::string binBase = target.filename().string();
            const std::string ext = fn->externLang == "golang" ? ".go" : ".rs";
            skelPath = target.parent_path() / (binBase + ext);
            stubContent = fn->externLang == "golang"
                ? renderGoStub(fn, order, classes, srcBase, binBase)
                : renderRustStub(fn, order, classes, srcBase, binBase);
            skelContent = fn->externLang == "golang"
                ? renderGoSkeleton(fn, classes)
                : renderRustSkeleton(fn, classes);
        }

        std::error_code exc;
        const bool stubExists = fs::exists(stubPath, exc);
        const std::string existing = stubExists ? readWhole(stubPath) : std::string();
        // Ownership guard: a differing stub owned by ANOTHER .dr must not be
        // overwritten - last-sync-wins would zero-fill the loser's args silently.
        if (stubExists && existing != stubContent) {
            const std::string owner = stubOwner(existing);
            if (owner.empty()) {
                std::cerr << "dragon ffi sync: " << stubPath.string()
                          << " exists but was not generated by dragon ffi sync; refusing to overwrite\n";
                return 1;
            }
            if (owner != srcBase) {
                std::cerr << "dragon ffi sync: \"" << fn->externPath
                          << "\" already has stubs generated from " << owner << "; " << srcBase
                          << " cannot also target it - one script/binary per extern.\n"
                          << "If " << owner << " no longer declares this extern, delete "
                          << stubPath.string() << " and re-run.\n";
                return 1;
            }
        }
        if (checkOnly) {
            if (existing != stubContent)
                stale.push_back(fn->name + ": " + stubPath.string());
            continue;
        }
        if (existing != stubContent) {
            if (!writeWhole(stubPath, stubContent)) {
                std::cerr << "dragon ffi sync: cannot write " << stubPath.string() << "\n";
                return 1;
            }
            std::cout << "wrote " << stubPath.string() << "\n";
        } else {
            std::cout << "up-to-date " << stubPath.string() << "\n";
        }
        std::error_code ec;
        if (!fs::exists(skelPath, ec)) {
            if (!writeWhole(skelPath, skelContent)) {
                std::cerr << "dragon ffi sync: cannot write " << skelPath.string() << "\n";
                return 1;
            }
            std::cout << "wrote " << skelPath.string() << " (skeleton - yours to edit)\n";
        }
    }

    if (usedPaths.empty()) {
        std::cerr << "dragon ffi sync: no process externs (extern \"python\"/\"golang\"/\"rust\") in "
                  << srcBase << "\n";
        return 1;
    }
    if (checkOnly && !stale.empty()) {
        for (auto& s : stale)
            std::cerr << "ffi stubs out of sync with declarations (" << s
                      << "). Run: dragon ffi sync " << filename << "\n";
        return 1;
    }
    return 0;
}

int verifyFfiStubSignatures(const Module& module) {
    int staleCount = 0;
    const fs::path srcDir = fs::path(module.filename).parent_path();
    for (auto& stmt : module.body) {
        auto* fn = dynamic_cast<const FunctionDecl*>(stmt.get());
        if (!fn || fn->externLang.empty()) continue;
        fs::path stubPath = stubPathFor(fn, srcDir);
        std::error_code ec;
        if (!fs::exists(stubPath, ec)) continue;  // stubless children are legit
        const std::string content = readWhole(stubPath);
        auto at = content.find("signature: ");
        if (at == std::string::npos) continue;    // not one of ours
        auto start = at + 11;
        auto eol = content.find('\n', start);
        std::string line = content.substr(start, eol - start);
        auto cut = line.find(" | build:");
        if (cut != std::string::npos) line = line.substr(0, cut);
        const std::string want = signatureOf(fn);
        if (line != want) {
            std::cerr << module.filename << ": ffi stub out of sync with declaration '"
                      << fn->name << "'\n  stub:        " << line
                      << "\n  declaration: " << want
                      << "\n  Run: dragon ffi sync " << module.filename << "\n";
            ++staleCount;
        }
    }
    return staleCount;
}

}  // namespace dragon
