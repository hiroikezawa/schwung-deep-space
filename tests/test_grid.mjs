/* Deep Space Convolution grid test: Schwung's REAL page controller and value formatter
 * (from a copy of the device's shared/ or a Schwung checkout), drawn through
 * Schwung's framebuffer harness, against a fake param store serving the built
 * DSP's real contract (tests/dump_contract.py). This is the screen the shadow
 * UI opens in a track slot and in Master FX alike.
 *
 *   DS_SHARED     a shared/ directory to take the page engine from (e.g. one
 *                 mirrored from the device); default <schwung>/src/shared
 *   SCHWUNG_ROOT  a Schwung checkout (for tools/param-pages/harness.mjs);
 *                 default ../schwung beside this repository
 *
 * Copyright (c) 2026 Hiro Ikezawa. MIT License (see LICENSE). */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = process.env.SCHWUNG_ROOT || path.resolve(here, "../../schwung");
const shared = process.env.DS_SHARED || path.join(root, "src/shared");
const load = (p) => import(pathToFileURL(p).href);
const { createController } = await load(path.join(shared, "param_pages/page_controller.mjs"));
const { decodeInput, applyInput } = await load(path.join(shared, "param_pages/page_input.mjs"));
const { formatParamValue } = await load(path.join(shared, "param_format.mjs"));
const { createFramebuffer, drawContext } = await load(path.join(root, "tools/param-pages/harness.mjs"));

let failures = 0;
function check(cond, msg) {
    if (!cond) { failures++; console.error("FAIL:", msg); }
    else console.log("ok  ", msg.split(":")[0]);
}

const KEYS = ["space", "depth", "size", "decay", "diffusion", "darkness", "freeze", "mix"];
const contract = JSON.parse(fs.readFileSync(path.join(here, "../build/native/contract.json"), "utf8"));
const manifest = JSON.parse(fs.readFileSync(path.join(here, "../module.json"), "utf8"));
const meta = Object.fromEntries(JSON.parse(contract.chain_params).map((p) => [p.key, p]));
const values = { ...contract.values };
const writes = [];
const bare = (k) => k.slice(k.indexOf(":") + 1);
const ctrl = createController({
    getParam: (full) => {
        const k = bare(full);
        if (k === "ui_hierarchy") return contract.ui_hierarchy;
        if (k === "chain_params") return contract.chain_params;
        if (k.endsWith(":effective") || k.endsWith(":modulated")) return null;
        return k in values ? values[k] : "";
    },
    setParam: (full, v) => {
        const k = bare(full);
        writes.push([k, String(v)]);
        if (k in values) {
            const m = meta[k];
            if (m.type === "enum") {   /* as the plugin: a name or an index, served as the name */
                const i = m.options.indexOf(String(v).toUpperCase());
                const n = i >= 0 ? i : Math.round(Number(v));
                values[k] = m.options[Math.max(0, Math.min(m.options.length - 1, n))];
            } else {
                values[k] = Math.max(m.min, Math.min(m.max, Number(v))).toFixed(4);
            }
        }
        return true;
    },
    now: () => clock,
});
let clock = 1000;
const fb = createFramebuffer();
const ctx = drawContext(fb);
ctrl.load({ slot: 0, component: "fx1", prefix: "fx1" });
function ticks(n) {
    for (let i = 0; i < n; i++) {
        clock += 17;
        ctrl.setLayout("movy");
        ctrl.tick();
        fb.clearScreen();
        ctrl.render(ctx, {});
        ctrl.renderOverlays(ctx, { clearScreen: () => fb.clearScreen() });
    }
}
const shot = (name) => {
    const dir = path.join(here, "../build/screens");
    fs.mkdirSync(dir, { recursive: true });
    if (fb.toPng) fs.writeFileSync(path.join(dir, name + ".png"), fb.toPng(4));
    else if (fb.writePng) fb.writePng(path.join(dir, name + ".png"));
};
ticks(40);

check(JSON.stringify(ctrl.page.keys.map(bare)) === JSON.stringify(KEYS),
      "one page = SPACE DEPTH SIZE DECAY DIFFUSION DARKNESS FREEZE MIX: " + ctrl.page.keys);
check(fb.countLit() > 200, "the page draws: " + fb.countLit());
console.log(fb.toBlocks ? fb.toBlocks() : fb.toAscii());
shot("main");

/* the values read in their own units, straight from the DSP's answers */
const shown = Object.fromEntries(KEYS.map((k) => [k, formatParamValue(values[k], meta[k])]));
console.log("     values:", JSON.stringify(shown));
check(shown.space === "CATHEDRAL" && shown.freeze === "OFF", "enums read as names: " + shown.space + " " + shown.freeze);
check(shown.decay === "8.0 sec", "DECAY reads in seconds: " + shown.decay);
check(/^1\.50 ?x$/.test(shown.size), "SIZE reads as a factor: " + shown.size);
check(shown.depth === "60%" && shown.mix === "40%" && shown.diffusion === "70%" && shown.darkness === "40%",
      "the rest read in %");

/* every knob writes its own key, within its range */
for (let k = 0; k < 8; k++) {
    const w0 = writes.length;
    for (let i = 0; i < 5; i++) { ctrl.tick(); applyInput(ctrl, decodeInput([0xb0, 71 + k, 1], {}), { nowMs: (clock += 17) }); }
    ticks(10);
    const mine = writes.slice(w0).filter(([key]) => key === KEYS[k]);
    const m = meta[KEYS[k]];
    const inRange = ([, v]) => m.type === "enum" ? m.options.includes(String(v).toUpperCase()) ||
        (Number(v) >= 0 && Number(v) < m.options.length) : Number(v) >= m.min && Number(v) <= m.max;
    check(mine.length > 0 && mine.every(inRange), `knob ${k + 1} = ${KEYS[k]}: ` + writes.slice(w0));
}
applyInput(ctrl, decodeInput([0x90, 3, 127], {}), { nowMs: clock }); ticks(4);   /* touch DECAY */
shot("touch_decay");
console.log(fb.toBlocks ? fb.toBlocks() : fb.toAscii());
applyInput(ctrl, decodeInput([0x90, 3, 0], {}), { nowMs: clock }); ticks(80);
applyInput(ctrl, decodeInput([0x90, 0, 127], {}), { nowMs: clock }); ticks(4);   /* touch SPACE */
shot("touch_space");
console.log(fb.toBlocks ? fb.toBlocks() : fb.toAscii());

check(!("ui_chain" in manifest), "no module screen: the standard grid everywhere (Master FX too)");
check(values.space !== "CATHEDRAL" && ["OFF", "ON"].includes(values.freeze), "SPACE moved off CATHEDRAL, FREEZE is OFF/ON: " + values.space + " " + values.freeze);
check(JSON.parse(contract.ui_hierarchy).levels.root.knobs.every((k) => k in meta), "every knob is in chain_params");

if (failures) { console.error(`${failures} GRID FAILURE(S)`); process.exit(1); }
console.log("ALL GRID TESTS PASSED");
