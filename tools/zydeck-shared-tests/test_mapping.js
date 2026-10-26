// The Mixxx mapping's JavaScript (res/controllers/Zydek-Tablet.script.js) against cases.json: its v7 values
// the same as the hub's C++. Runs in Node (CI) or JavaScriptCore's jsc (a Mac):
//   node test_mapping.js <cases.json> <mapping.js>         jsc test_mapping.js -- <cases.json> <mapping.js>
/* global load, read, readFile, arguments, print, quit */
var node = typeof process !== "undefined" && process.versions && process.versions.node;
var args = node ? process.argv.slice(2) : (typeof arguments !== "undefined" ? Array.prototype.slice.call(arguments) : []);
var text = function (path) { return node ? require("fs").readFileSync(path, "utf8") : readFile(path); };
var say = node ? console.log : print;
var cases = JSON.parse(text(args[0]));
var TabletPads = (node ? require("vm").runInThisContext : (0, eval))(text(args[1]) + "\n;TabletPads");
var wrong = 0;
cases.values.forEach(function (c) {
    var bytes = TabletPads.encodeValue(c[0]);
    if (JSON.stringify(bytes) !== JSON.stringify(c[1])) {
        wrong++;
        say("JavaScript WRONG: encodeValue(" + c[0] + ") = " + JSON.stringify(bytes) + ", not " + JSON.stringify(c[1]));
    }
    var back = TabletPads.decodeValue(c[1], 0);
    if (back !== c[2]) {
        wrong++;
        say("JavaScript WRONG: decodeValue(bytes of " + c[0] + ") = " + back + ", not " + c[2]);
    }
});
say("JavaScript: " + (wrong ? "WRONG" : "all right"));
if (node) { process.exitCode = wrong ? 1 : 0; } else if (wrong && typeof quit === "function") { quit(1); }
