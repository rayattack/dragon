// parsecount.js - reference AST kind counts from real tsc (TS 5.9 JS API).
// Usage: node parsecount.js <file> [--bench N]
const ts = require("typescript");
const fs = require("fs");

const NAMES = ["ArrowFunction","AsExpression","BinaryExpression","Block","CallExpression","ClassDeclaration","ConditionalExpression","ConditionalType","Constructor","Decorator","EnumDeclaration","ExportDeclaration","ForOfStatement","FunctionDeclaration","FunctionExpression","GetAccessor","IfStatement","ImportDeclaration","InterfaceDeclaration","IntersectionType","MappedType","MethodDeclaration","ModuleDeclaration","NewExpression","NumericLiteral","ObjectLiteralExpression","PropertyAccessExpression","PropertyDeclaration","RegularExpressionLiteral","ReturnStatement","SatisfiesExpression","SetAccessor","StringLiteral","SwitchStatement","TemplateExpression","TryStatement","TupleType","TypeAliasDeclaration","TypeReference","UnionType","VariableDeclaration","VariableStatement"];

const file = process.argv[2];
const src = fs.readFileSync(file, "utf8");

function parseOnce() {
  return ts.createSourceFile(file, src, ts.ScriptTarget.Latest, /*setParentNodes*/ false, ts.ScriptKind.TS);
}

if (process.argv[3] === "--bench") {
  const iters = parseInt(process.argv[4] || "20", 10);
  let best = Infinity, stmts = 0;
  for (let i = 0; i < iters; i++) {
    const t0 = process.hrtime.bigint();
    const sf = parseOnce();
    const dt = Number(process.hrtime.bigint() - t0) / 1e6;
    stmts = sf.statements.length;
    if (dt < best) best = dt;
  }
  console.log(`statements: ${stmts}`);
  console.log(`best: ${best.toFixed(3)} ms`);
} else {
  const sf = parseOnce();
  const counts = {};
  function visit(node) {
    counts[node.kind] = (counts[node.kind] || 0) + 1;
    ts.forEachChild(node, visit);
  }
  ts.forEachChild(sf, visit);
  const out = [`statements ${sf.statements.length}`];
  for (const nm of NAMES) {
    const k = ts.SyntaxKind[nm];
    out.push(`${nm} ${counts[k] || 0}`);
  }
  console.log(out.join("\n"));
}
