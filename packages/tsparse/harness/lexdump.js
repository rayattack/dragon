// lexdump.js - reference raw token stream from the real tsc (TS 5.9 JS API).
// Usage: node lexdump.js <file> [--bench N]
const ts = require("typescript");
const fs = require("fs");

const NAMES = ["Unknown","EndOfFileToken","NumericLiteral","BigIntLiteral","StringLiteral","RegularExpressionLiteral","NoSubstitutionTemplateLiteral","TemplateHead","TemplateMiddle","TemplateTail","OpenBraceToken","CloseBraceToken","OpenParenToken","CloseParenToken","OpenBracketToken","CloseBracketToken","DotToken","DotDotDotToken","SemicolonToken","CommaToken","QuestionDotToken","LessThanToken","LessThanSlashToken","GreaterThanToken","LessThanEqualsToken","GreaterThanEqualsToken","EqualsEqualsToken","ExclamationEqualsToken","EqualsEqualsEqualsToken","ExclamationEqualsEqualsToken","EqualsGreaterThanToken","PlusToken","MinusToken","AsteriskToken","AsteriskAsteriskToken","SlashToken","PercentToken","PlusPlusToken","MinusMinusToken","LessThanLessThanToken","GreaterThanGreaterThanToken","GreaterThanGreaterThanGreaterThanToken","AmpersandToken","BarToken","CaretToken","ExclamationToken","TildeToken","AmpersandAmpersandToken","BarBarToken","QuestionToken","ColonToken","AtToken","QuestionQuestionToken","BacktickToken","HashToken","EqualsToken","PlusEqualsToken","MinusEqualsToken","AsteriskEqualsToken","AsteriskAsteriskEqualsToken","SlashEqualsToken","PercentEqualsToken","LessThanLessThanEqualsToken","GreaterThanGreaterThanEqualsToken","GreaterThanGreaterThanGreaterThanEqualsToken","AmpersandEqualsToken","BarEqualsToken","BarBarEqualsToken","AmpersandAmpersandEqualsToken","QuestionQuestionEqualsToken","CaretEqualsToken","Identifier","PrivateIdentifier","AbstractKeyword","AccessorKeyword","AnyKeyword","AsKeyword","AssertsKeyword","AssertKeyword","AsyncKeyword","AwaitKeyword","BigIntKeyword","BooleanKeyword","BreakKeyword","CaseKeyword","CatchKeyword","ClassKeyword","ConstKeyword","ConstructorKeyword","ContinueKeyword","DebuggerKeyword","DeclareKeyword","DefaultKeyword","DeleteKeyword","DoKeyword","ElseKeyword","EnumKeyword","ExportKeyword","ExtendsKeyword","FalseKeyword","FinallyKeyword","ForKeyword","FromKeyword","FunctionKeyword","GetKeyword","GlobalKeyword","IfKeyword","ImplementsKeyword","ImportKeyword","InKeyword","InferKeyword","InstanceOfKeyword","InterfaceKeyword","IntrinsicKeyword","IsKeyword","KeyOfKeyword","LetKeyword","ModuleKeyword","NamespaceKeyword","NeverKeyword","NewKeyword","NullKeyword","NumberKeyword","ObjectKeyword","OfKeyword","OutKeyword","OverrideKeyword","PackageKeyword","PrivateKeyword","ProtectedKeyword","PublicKeyword","ReadonlyKeyword","RequireKeyword","ReturnKeyword","SatisfiesKeyword","SetKeyword","StaticKeyword","StringKeyword","SuperKeyword","SwitchKeyword","SymbolKeyword","ThisKeyword","ThrowKeyword","TrueKeyword","TryKeyword","TypeKeyword","TypeOfKeyword","UndefinedKeyword","UniqueKeyword","UnknownKeyword","UsingKeyword","VarKeyword","VoidKeyword","WhileKeyword","WithKeyword","YieldKeyword","DeferKeyword"];

// Forward-map my canonical names onto this TS version's kind numbers
// (reverse enum lookup is unreliable: aliases like FirstAssignment shadow it).
const kindToName = {};
let missing = [];
for (const nm of NAMES) {
  const v = ts.SyntaxKind[nm];
  if (v === undefined) { missing.push(nm); continue; }
  kindToName[v] = nm;
}
if (missing.length) console.error("NOT IN ts.SyntaxKind:", missing.join(","));

const file = process.argv[2];
const src = fs.readFileSync(file, "utf8");

function scanAll() {
  const sc = ts.createScanner(ts.ScriptTarget.Latest, /*skipTrivia*/ true, ts.LanguageVariant.Standard, src);
  let count = 0;
  while (true) {
    const k = sc.scan();
    count++;
    if (k === ts.SyntaxKind.EndOfFileToken) break;
  }
  return count;
}

if (process.argv[3] === "--bench") {
  const iters = parseInt(process.argv[4] || "20", 10);
  let best = Infinity, tokens = 0;
  for (let i = 0; i < iters; i++) {
    const t0 = process.hrtime.bigint();
    tokens = scanAll();
    const dt = Number(process.hrtime.bigint() - t0) / 1e6;
    if (dt < best) best = dt;
  }
  console.log(`tokens: ${tokens}`);
  console.log(`best: ${best.toFixed(3)} ms`);
} else {
  const sc = ts.createScanner(ts.ScriptTarget.Latest, true, ts.LanguageVariant.Standard, src);
  const out = [];
  while (true) {
    const k = sc.scan();
    out.push(`${kindToName[k] || ("K" + k)} ${sc.getTokenStart()} ${sc.getTokenEnd()}`);
    if (k === ts.SyntaxKind.EndOfFileToken) break;
  }
  console.log(out.join("\n"));
}
