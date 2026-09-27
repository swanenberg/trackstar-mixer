// Robot DJ — Mixxx-kant van de koppeling ("handen").
// Protocol: SysEx  F0 7D <ASCII-JSON> F7  in beide richtingen.
//   in : {"id":n,"cmd":"ping"|"get"|"getmany"|"set"|"setp"|"track"|"navknob"|"load"|"stream"|"focus"|
//         "seek"|"choreo"|"abort"|"reset"|"txstatus", ...}
//   uit: {"id":n, ...antwoord}  of  {"ev":"hello"|"track_loaded"|"play"|"pos"|"tx", ...}
// De Python-daemon (robotdj/link.py) maakt de virtuele poort "RobotDJ"; Mixxx moet
// NA de daemon starten (MIDI-apparaten worden alleen bij het opstarten gescand).
//
// Choreografie ("choreo"): plan = {
//   name: "drums_swap", from: "[Channel1]", to: "[Channel2]",
//   at: <stereo-samplepositie in FROM = de "1" waarop beat 0 valt>,
//   toPos: <stereo-samplepositie in TO om vanaf te starten>, beatLen: <stereo-samples per beat in FROM>,
//   sync: true, keySync: true,
//   prep:  [[group, key, value], ...]                       // direct bij wapenen (TO klaarzetten)
//   steps: [{b: <beat t.o.v. at>, set: [[g,k,v],...], ramp: [{g,k,from,to,beats,curve}], play: g, stop: g}, ...]
//   end: <beat waarop de choreografie klaar is>              // dan opruimen (alles wat we aanraakten terug)
// }
// Vóór beat 0 telt het script mee op de beat_active van FROM (catch-up als we laat gewapend zijn);
// vanaf de stap met `play` loopt de klok op wandtijd (beat-ms uit FROM's tempo). Rampen: 20 ms-tick.
// Alles wat via set/ramp is aangeraakt wordt aan het einde teruggezet (behalve play/positie/loops/rate),
// loops die wij aanzetten gaan uit, en FROM wordt neutraal (volume 1, EQ vlak, stems open).

var RobotDJ = {};
RobotDJ.VERSION = "0.4.0";
RobotDJ.connections = [];
RobotDJ.posTimer = 0;
RobotDJ.streamGroups = [];
RobotDJ.tx = null;   // lopende choreografie
RobotDJ.NO_RESTORE = {play: 1, playposition: 1, beatloop_activate: 1, reloop_toggle: 1, beatlooproll_activate: 1,
                      sync_enabled: 1, sync_mode: 1, sync_key: 1, quantize: 1, rate: 1, rate_ratio: 1,
                      beatloop_size: 1, loop_enabled: 1};
// hotcue_X_gotoandloop (opgeslagen loops) wordt ook niet teruggezet; zie RobotDJ.setv


// ---- eigen schrijfacties onthouden (handen-detectie) ---------------------------------
// Elke waarde die wij zetten gaat hierlangs. Meldt Mixxx daarna een ANDERE waarde op zo'n
// control, dan was dat een mens (fader, knop, FLX4) → {"ev":"human"} naar de Bot.
RobotDJ.lastSet = {};
RobotDJ.humanOn = true;
RobotDJ.lastHuman = 0;
RobotDJ.setv = function(g, k, v) {
    RobotDJ.lastSet[g + "," + k] = v;
    engine.setValue(g, k, v);
};
RobotDJ.onHuman = function(g, k, v) {
    if (!RobotDJ.humanOn) { return; }
    var last = RobotDJ.lastSet[g + "," + k];
    if (last !== undefined && Math.abs(last - v) < 1e-6) { return; }        // dat waren wij
    if (k === "play" && v === 0 && engine.getValue(g, "playposition") >= 0.98) { return; }   // plaat was op
    if (k === "play" && last === undefined) { return; }                     // eerste play na laden: Mixxx zelf
    var now = Date.now();
    if (now - RobotDJ.lastHuman < 400) { return; }
    RobotDJ.lastHuman = now;
    RobotDJ.send({ev: "human", group: g, key: k, value: v});
};
RobotDJ.watchHuman = function(g, k) {
    RobotDJ.connections.push(engine.makeConnection(g, k, function(v) { RobotDJ.onHuman(g, k, v); }));
};
// Knop in de Mixer-skin: [TrackStar],handoff (aan = jij neemt over, uit = bot verder).
// De skin maakt die control aan; als het script eerder laadt dan de skin, later nog eens proberen.
RobotDJ.handoffTries = 0;
RobotDJ.watchHandoff = function() {
    var c = engine.makeConnection("[TrackStar]", "handoff", function(v) {
        RobotDJ.send({ev: "handoff_button", value: v});
    });
    if (c && c.isConnected) { RobotDJ.connections.push(c); return; }
    if (RobotDJ.handoffTries++ < 15) { engine.beginTimer(2000, RobotDJ.watchHandoff, true); }
};

RobotDJ.init = function(id, debugging) {
    RobotDJ.id = id;
    ["[Channel1]", "[Channel2]"].forEach(function(g) {
        RobotDJ.connections.push(engine.makeConnection(g, "track_loaded", function(v) {
            delete RobotDJ.lastSet[g + ",play"];          // nieuwe plaat: play-historie vergeten
            RobotDJ.send({ev: "track_loaded", group: g, value: v, track: RobotDJ.trackInfo(g)});
        }));
        RobotDJ.connections.push(engine.makeConnection(g, "play", function(v) {
            RobotDJ.send({ev: "play", group: g, value: v});
            RobotDJ.onHuman(g, "play", v);
        }));
        RobotDJ.watchHuman(g, "volume");
        RobotDJ.watchHuman(RobotDJ.eqGroup(g), "parameter1");
        RobotDJ.watchHuman(RobotDJ.eqGroup(g), "parameter2");
        RobotDJ.watchHuman(RobotDJ.eqGroup(g), "parameter3");
        RobotDJ.watchHuman(RobotDJ.qfxGroup(g), "super1");
    });
    RobotDJ.watchHuman("[Master]", "crossfader");
    RobotDJ.watchHandoff();
    RobotDJ.send({ev: "hello", version: RobotDJ.VERSION});
};

RobotDJ.shutdown = function() {
    RobotDJ.abort("shutdown");
    RobotDJ.connections.forEach(function(c) { c.disconnect(); });
    RobotDJ.connections = [];
    if (RobotDJ.posTimer) { engine.stopTimer(RobotDJ.posTimer); RobotDJ.posTimer = 0; }
};

RobotDJ.samplePos = function(g) {
    return engine.getValue(g, "playposition") * engine.getValue(g, "track_samples");
};

RobotDJ.trackInfo = function(g) {
    return {
        loaded: engine.getValue(g, "track_loaded"),
        duration: engine.getValue(g, "duration"),
        bpm: engine.getValue(g, "file_bpm"),
        samples: engine.getValue(g, "track_samples"),
        samplerate: engine.getValue(g, "track_samplerate"),
        key: engine.getValue(g, "file_key"),
        stems: engine.getValue(g, "stem_count"),
        play: engine.getValue(g, "play"),
        pos: engine.getValue(g, "playposition"),
        sample: RobotDJ.samplePos(g),
        rate: engine.getValue(g, "rate")
    };
};

// Alles buiten printbaar ASCII wordt als \uXXXX ge-escaped, zodat de SysEx-bytes 7-bit blijven.
RobotDJ.send = function(obj) {
    var s = JSON.stringify(obj).replace(/[^ -~]/g, function(c) {
        return "\\u" + ("0000" + c.charCodeAt(0).toString(16)).slice(-4);
    });
    var bytes = [0xF0, 0x7D];
    for (var i = 0; i < s.length; i++) { bytes.push(s.charCodeAt(i) & 0x7F); }
    bytes.push(0xF7);
    midi.sendSysexMsg(bytes, bytes.length);
};

RobotDJ.chunks = null;
RobotDJ.incomingData = function(data, length) {
    var s = "", i;
    if (data[2] === 0x01) {                     // stuk k van n: F0 7D 01 k n <ascii> F7
        var k = data[3], n = data[4];
        if (k === 0 || !RobotDJ.chunks) { RobotDJ.chunks = []; }
        var part = "";
        for (i = 5; i < length - 1; i++) { part += String.fromCharCode(data[i]); }
        RobotDJ.chunks[k] = part;
        if (RobotDJ.chunks.length < n || RobotDJ.chunks.indexOf(undefined) >= 0) { return; }
        s = RobotDJ.chunks.join("");
        RobotDJ.chunks = null;
    } else {
        for (i = 2; i < length - 1; i++) { s += String.fromCharCode(data[i]); }
    }
    var msg;
    try { msg = JSON.parse(s); } catch (e) { RobotDJ.send({err: "bad json", raw: s.slice(0, 80)}); return; }
    try { RobotDJ.handle(msg); } catch (e) { RobotDJ.send({id: msg.id, err: String(e)}); }
};

RobotDJ.handle = function(m) {
    var r = {id: m.id};
    switch (m.cmd) {
    case "ping":
        r.pong = true; r.version = RobotDJ.VERSION; r.tx = RobotDJ.tx ? RobotDJ.tx.phase : null; break;
    case "get":
        r.value = engine.getValue(m.group, m.key); break;
    case "getmany":
        r.values = {};
        m.items.forEach(function(it) { r.values[it[0] + "," + it[1]] = engine.getValue(it[0], it[1]); });
        break;
    case "set":
        RobotDJ.setv(m.group, m.key, m.value); r.ok = true; break;
    case "setp":
        engine.setParameter(m.group, m.key, m.value); r.ok = true; break;
    case "track":
        r.track = RobotDJ.trackInfo(m.group); break;
    case "focus":
        RobotDJ.setv("[Library]", "focused_widget", m.value); r.ok = true; break;
    case "navknob":
        RobotDJ.setv("[Playlist]", "SelectTrackKnob", m.delta); r.ok = true; break;
    case "load":
        RobotDJ.setv(m.group, "LoadSelectedTrack", 1); r.ok = true; break;
    case "seek":
        RobotDJ.setv(m.group, "playposition", m.sample / engine.getValue(m.group, "track_samples"));
        r.ok = true; r.sample = RobotDJ.samplePos(m.group); break;
    case "stream":
        RobotDJ.setStream(m.interval || 250, m.groups || ["[Channel1]", "[Channel2]"]); r.ok = true; break;
    case "choreo":
        r.ok = RobotDJ.startChoreo(m.plan); r.tx = RobotDJ.tx ? RobotDJ.tx.phase : null; break;
    case "abort":
        RobotDJ.abort("abort"); r.ok = true; break;
    case "release":
        r.released = RobotDJ.release(); r.ok = true; break;
    case "hurry":
        r.ok = RobotDJ.hurry(); break;
    case "humandetect":
        RobotDJ.humanOn = !!m.on; r.ok = true; break;
    case "reset":
        RobotDJ.neutral(m.group); r.ok = true; break;
    case "txstatus":
        r.tx = RobotDJ.tx ? {phase: RobotDJ.tx.phase, name: RobotDJ.tx.plan.name, beat: RobotDJ.tx.beat} : null; break;
    default:
        r.err = "unknown cmd " + m.cmd;
    }
    RobotDJ.send(r);
};

RobotDJ.setStream = function(interval, groups) {
    if (RobotDJ.posTimer) { engine.stopTimer(RobotDJ.posTimer); RobotDJ.posTimer = 0; }
    RobotDJ.streamGroups = groups;
    if (interval <= 0 || groups.length === 0) { return; }
    RobotDJ.posTimer = engine.beginTimer(interval, function() {
        var d = {};
        RobotDJ.streamGroups.forEach(function(g) {
            d[g] = {play: engine.getValue(g, "play"), sample: RobotDJ.samplePos(g),
                    bd: engine.getValue(g, "beat_distance"), bpm: engine.getValue(g, "bpm"),
                    vol: engine.getValue(g, "volume")};
        });
        RobotDJ.send({ev: "pos", d: d});
    });
};

// ---- helpers ------------------------------------------------------------------
RobotDJ.eqGroup = function(g) { return "[EqualizerRack1_" + g + "_Effect1]"; };
RobotDJ.qfxGroup = function(g) { return "[QuickEffectRack1_" + g + "]"; };
RobotDJ.stemGroup = function(g, k) { return "[" + g.substring(1, g.length - 1) + "_Stem" + k + "]"; };

// Alles terug naar normaal (volume, EQ, filter, stems, loop, sync) — behalve het TEMPO:
// dat is een muzikale keuze die blijft staan.
RobotDJ.neutral = function(g) {
    RobotDJ.setv(g, "volume", 1);
    var eq = RobotDJ.eqGroup(g);
    RobotDJ.setv(eq, "parameter1", 1);
    RobotDJ.setv(eq, "parameter2", 1);
    RobotDJ.setv(eq, "parameter3", 1);
    RobotDJ.setv(RobotDJ.qfxGroup(g), "super1", 0.5);
    for (var k = 1; k <= 4; k++) {
        RobotDJ.setv(RobotDJ.stemGroup(g, k), "volume", 1);
        RobotDJ.setv(RobotDJ.stemGroup(g, k), "mute", 0);
    }
    if (engine.getValue(g, "loop_enabled")) { RobotDJ.setv(g, "reloop_toggle", 1); }
    RobotDJ.setv(g, "sync_mode", 0);
    RobotDJ.setv(g, "sync_enabled", 0);
};

// Hoe ver staat dit deck van zijn eigen dichtstbijzijnde beat, in ECHTE seconden?
// (beat_closest = samplepositie van de dichtstbijzijnde beat; rate_ratio = afspeelsnelheid)
RobotDJ.beatOffsetSec = function(g) {
    var bc = engine.getValue(g, "beat_closest");
    if (bc < 0) { return null; }
    var sr = engine.getValue(g, "track_samplerate") * 2;
    return (RobotDJ.samplePos(g) - bc) / sr / engine.getValue(g, "rate_ratio");
};

// Faseverschil tussen A en B in beats van A. Tempo-onafhankelijk, dus ook goed bij half/dubbel tempo.
RobotDJ.phaseOffset = function(tx) {
    var oa = RobotDJ.beatOffsetSec(tx.A), ob = RobotDJ.beatOffsetSec(tx.B);
    if (oa === null || ob === null) { return null; }
    var beatSecA = 60 / engine.getValue(tx.A, "bpm");
    var off = (ob - oa) / beatSecA;
    while (off > 0.5) { off -= 1; }
    while (off < -0.5) { off += 1; }
    return off;
};

// Meten en zo nodig bijdraaien (beatsync_phase = fase pakken zonder het tempo los te laten).
RobotDJ.checkPhase = function(tx, tries) {
    // Alleen zinvol zolang BEIDE decks spelen (bij bv. echo_out is A al gestopt).
    if (RobotDJ.tx !== tx || !engine.getValue(tx.A, "play") || !engine.getValue(tx.B, "play")) { return; }
    var off = RobotDJ.phaseOffset(tx);
    if (off === null) { return; }
    var fixing = Math.abs(off) > 0.08 && tries < 1;   // kleiner: bijdraaien schiet door; blijft het
    // staan, dan klopt het grid van een van de twee niet en helpt nog een poging ook niet
    RobotDJ.send({ev: "tx", phase: "phase", off: off, fixing: fixing, tries: tries,
                  bpmA: engine.getValue(tx.A, "bpm"), bpmB: engine.getValue(tx.B, "bpm")});
    if (fixing) {
        RobotDJ.setv(tx.B, "beatsync_phase", 1);
        engine.beginTimer(700, function() { RobotDJ.checkPhase(tx, tries + 1); }, true);
    }
};

RobotDJ.curve = function(name, p) {
    switch (name) {
    case "sin": return Math.sin(p * Math.PI / 2);          // opkomen, constant power
    case "cos": return 1 - Math.cos(p * Math.PI / 2);      // wegvallen (gebruik from>to), constant power
    case "exp": return p * p;
    case "log": return Math.sqrt(p);
    default: return p;                                     // lin
    }
};

// ---- choreografie ---------------------------------------------------------------
RobotDJ.startChoreo = function(plan) {
    if (RobotDJ.tx) { RobotDJ.abort("replaced"); }
    var A = plan.from, B = plan.to;
    if (!engine.getValue(A, "play") || !engine.getValue(B, "track_loaded")) {
        RobotDJ.send({ev: "tx", phase: "refused", reason: "FROM speelt niet of TO is leeg"});
        return false;
    }
    var steps = (plan.steps || []).slice().sort(function(x, y) { return x.b - y.b; });
    var tx = {plan: plan, A: A, B: B, phase: "armed", steps: steps, next: 0, beat: null,
              fired: false, clock: false, t0: 0, beatMs: 0, ramps: [], touched: {}, order: [],
              timer: 0, beatConn: null, playConn: null, guard: 0, loopsOn: {}};
    RobotDJ.tx = tx;
    tx.beatMs = plan.beatLen / (2 * engine.getValue(A, "track_samplerate")) / engine.getValue(A, "rate_ratio") * 1000;
    // TO klaarzetten
    RobotDJ.setv(B, "play", 0);
    RobotDJ.setv(B, "quantize", 1);
    if (plan.sync !== false) {
        // sync LOCK i.p.v. één druk op sync: de follower blijft in tempo én fase hangen, ook als
        // we het tempo van de leider tijdens de move opschroeven, en lijnt bij play op de "1" uit.
        RobotDJ.setv(A, "sync_mode", 2);   // leider
        RobotDJ.setv(B, "sync_mode", 1);   // volger
    }
    if (plan.keySync) { RobotDJ.setv(B, "sync_key", 1); }
    (plan.prep || []).forEach(function(s) { RobotDJ.touch(tx, s[0], s[1], s[2]); });
    engine.beginTimer(40, function() {
        RobotDJ.setv(B, "playposition", plan.toPos / engine.getValue(B, "track_samples"));
        engine.beginTimer(60, function() {
            if (plan.sync !== false) { RobotDJ.setv(B, "beatsync_phase", 1); }
        }, true);
    }, true);
    // beat-teller op FROM tot het vuren
    tx.beatConn = engine.makeConnection(A, "beat_active", function(v) {
        if (v !== 1 || tx.clock) { return; }
        var b = Math.round((RobotDJ.samplePos(A) - plan.at) / plan.beatLen);
        tx.beat = b;
        RobotDJ.runDue(tx, b);
    });
    tx.playConn = engine.makeConnection(A, "play", function(v) {
        if (v === 0 && !tx.clock) { RobotDJ.abort("bron-deck gestopt"); }
    });
    tx.timer = engine.beginTimer(20, function() { RobotDJ.tick(tx); });
    var bNow = (RobotDJ.samplePos(A) - plan.at) / plan.beatLen;
    tx.guard = engine.beginTimer(Math.max(5000, (-bNow) * tx.beatMs * 2 + 15000), function() {
        if (RobotDJ.tx === tx && !tx.clock) { RobotDJ.abort("timeout: beat 0 niet bereikt"); }
    }, true);
    RobotDJ.send({ev: "tx", phase: "armed", name: plan.name, beatsToGo: -bNow});
    if (bNow >= -0.5) { RobotDJ.runDue(tx, Math.round(bNow)); }   // laat gewapend: inhalen
    return true;
};

// Compacte groepsnamen uit het plan: A/B, eqA/eqB, qfxA/qfxB, sA1..sB4, fx1; sleutels group_A_enable.
RobotDJ.expand = function(tx, g) {
    var A = tx.A, B = tx.B, m;
    if (g === "A") { return A; }
    if (g === "B") { return B; }
    if (g === "eqA") { return RobotDJ.eqGroup(A); }
    if (g === "eqB") { return RobotDJ.eqGroup(B); }
    if (g === "qfxA") { return RobotDJ.qfxGroup(A); }
    if (g === "qfxB") { return RobotDJ.qfxGroup(B); }
    if (g === "fx1") { return "[EffectRack1_EffectUnit1]"; }
    if ((m = /^s([AB])([1-4])$/.exec(g))) { return RobotDJ.stemGroup(m[1] === "A" ? A : B, m[2]); }
    return g;
};
RobotDJ.expandKey = function(tx, k) {
    return k.replace("group_A_enable", "group_" + tx.A + "_enable").replace("group_B_enable", "group_" + tx.B + "_enable");
};

RobotDJ.touch = function(tx, g, k, v) {
    g = RobotDJ.expand(tx, g); k = RobotDJ.expandKey(tx, k);
    var id = g + "," + k;
    if (!(id in tx.touched) && !RobotDJ.NO_RESTORE[k] && !/^hotcue_\d+_/.test(k)) {
        tx.touched[id] = engine.getValue(g, k);
        tx.order.push([g, k]);
    }
    if ((k === "beatloop_activate" || /_gotoandloop$/.test(k)) && v) { tx.loopsOn[g] = true; }
    RobotDJ.setv(g, k, v);
};

// Vanaf beat 0 loopt de telling op de KLOK, niet meer op A's positie: moves mogen A daarna laten
// springen of loopen (hook_loop, loop_build) zonder dat de telling blijft hangen. Voorheen gebeurde
// dit pas bij de eerste play-stap; bij hook_loop (A springt op 0, B start op 8) liep dat vast.
RobotDJ.startClock = function(tx, b) {
    tx.clock = true; tx.t0 = Date.now() - b * tx.beatMs; tx.phase = "running";
    if (tx.beatConn) { tx.beatConn.disconnect(); tx.beatConn = null; }
    if (tx.playConn) { tx.playConn.disconnect(); tx.playConn = null; }
    if (tx.guard) { engine.stopTimer(tx.guard); tx.guard = 0; }
};

RobotDJ.runDue = function(tx, b) {
    while (tx.next < tx.steps.length && tx.steps[tx.next].b <= b) {
        var st = tx.steps[tx.next++];
        if (!tx.clock && st.b >= 0) { RobotDJ.startClock(tx, b); }
        (st.set || []).forEach(function(s) { RobotDJ.touch(tx, s[0], s[1], s[2]); });
        (st.ramp || []).forEach(function(r) {
            var late = (b - st.b) * tx.beatMs;                     // ms te laat gestart (catch-up)
            var dur = Math.max(20, r.beats * tx.beatMs - late);
            var rg = RobotDJ.expand(tx, r.g), rk = RobotDJ.expandKey(tx, r.k);
            RobotDJ.touch(tx, rg, rk, r.from);
            tx.ramps.push({g: rg, k: rk, from: r.from, to: r.to, curve: r.curve || "lin", t0: Date.now(), dur: dur});
        });
        if (st.play) {
            RobotDJ.setv(RobotDJ.expand(tx, st.play), "play", 1);
            if (!tx.fired) {
                tx.fired = true;
                if (!tx.clock) { RobotDJ.startClock(tx, b); }
                RobotDJ.send({ev: "tx", phase: "started", name: tx.plan.name, late: b - st.b});
                engine.beginTimer(900, function() { RobotDJ.checkPhase(tx, 0); }, true);
            }
        }
        if (st.stop) { RobotDJ.setv(RobotDJ.expand(tx, st.stop), "play", 0); }
        if (st.scr) { RobotDJ.startScratch(tx, st.scr, (b - st.b) * tx.beatMs); }
        if (st.mark) { RobotDJ.send({ev: "tx", phase: "mark", mark: st.mark, beat: b}); }
    }
    if (tx.clock && b >= tx.plan.end) { RobotDJ.finish(tx); }
};

// ---- scratchen -----------------------------------------------------------------------
// scr = {g, beats, div, amp, cut}: de plaat gaat in slagen van `div` tellen heen en terug
// (snelheid = sinusboog, piek `amp` x normaal), dus netto blijft hij rond hetzelfde stukje.
// cut: "none" (baby), "back" (geluid alleen op de heenslag, chirp-achtig), "gate" (volume
// in zestienden aan/uit, transformer). Snelheid en cut worden elke tick (20 ms) gezet.
RobotDJ.startScratch = function(tx, scr, lateMs) {
    var g = RobotDJ.expand(tx, scr.g);
    RobotDJ.touch(tx, g, "volume", engine.getValue(g, "volume"));
    RobotDJ.setv(g, "scratch2_enable", 1);
    tx.scr = {g: g, div: scr.div || 0.5, amp: scr.amp || 2.5, cut: scr.cut || "none",
              t0: Date.now() - (lateMs || 0), dur: (scr.beats || 4) * tx.beatMs};
};

RobotDJ.stopScratch = function(tx) {
    var s = tx.scr;
    if (!s) { return; }
    RobotDJ.setv(s.g, "scratch2", 0);
    RobotDJ.setv(s.g, "scratch2_enable", 0);
    RobotDJ.setv(s.g, "volume", 1);
    tx.scr = null;
};

RobotDJ.scratchTick = function(tx, now) {
    var s = tx.scr, el = now - s.t0;
    if (el >= s.dur) { RobotDJ.stopScratch(tx); return; }
    var stroke = el / (s.div * tx.beatMs), k = Math.floor(stroke), frac = stroke - k;
    var dir = (k % 2 === 0) ? 1 : -1;
    RobotDJ.setv(s.g, "scratch2", dir * s.amp * Math.sin(Math.PI * frac));
    var open = true;
    if (s.cut === "back") { open = dir > 0; }
    else if (s.cut === "gate") { open = Math.floor(el / (tx.beatMs / 4)) % 2 === 0; }
    RobotDJ.setv(s.g, "volume", open ? 1 : 0);
};

RobotDJ.tick = function(tx) {
    var now = Date.now();
    if (tx.scr) { RobotDJ.scratchTick(tx, now); }
    for (var i = tx.ramps.length - 1; i >= 0; i--) {
        var r = tx.ramps[i];
        var p = Math.min(1, (now - r.t0) / r.dur);
        RobotDJ.setv(r.g, r.k, r.from + (r.to - r.from) * RobotDJ.curve(r.curve, p));
        if (p >= 1) { tx.ramps.splice(i, 1); }
    }
    if (tx.clock) {
        var b = (now - tx.t0) / tx.beatMs;
        tx.beat = b;
        RobotDJ.runDue(tx, b);
    }
};

RobotDJ.cleanup = function(tx) {
    RobotDJ.stopScratch(tx);
    if (tx.timer) { engine.stopTimer(tx.timer); tx.timer = 0; }
    if (tx.beatConn) { tx.beatConn.disconnect(); tx.beatConn = null; }
    if (tx.playConn) { tx.playConn.disconnect(); tx.playConn = null; }
    if (tx.guard) { engine.stopTimer(tx.guard); tx.guard = 0; }
    tx.ramps = [];
    // B eerst uit de sync halen: zolang hij volger is, sleept elke verandering aan A hem mee
    // (daardoor viel het tempo na een move terug naar dat van vóór de tempo-ramp).
    RobotDJ.setv(tx.B, "sync_mode", 0);
    RobotDJ.setv(tx.A, "sync_mode", 0);
    // aangeraakte controls terug in omgekeerde volgorde
    for (var i = tx.order.length - 1; i >= 0; i--) {
        var g = tx.order[i][0], k = tx.order[i][1];
        RobotDJ.setv(g, k, tx.touched[g + "," + k]);
    }
    for (var grp in tx.loopsOn) {
        if (engine.getValue(grp, "loop_enabled")) { RobotDJ.setv(grp, "reloop_toggle", 1); }
    }
};

RobotDJ.finish = function(tx) {
    RobotDJ.cleanup(tx);
    RobotDJ.setv(tx.A, "play", 0);
    RobotDJ.neutral(tx.A);
    RobotDJ.setv(tx.B, "volume", 1);
    RobotDJ.setv(tx.B, "sync_mode", 0);
    RobotDJ.setv(tx.B, "sync_enabled", 0);
    for (var k = 1; k <= 4; k++) { RobotDJ.setv(RobotDJ.stemGroup(tx.B, k), "volume", 1); }
    tx.phase = "done";
    RobotDJ.tx = null;
    RobotDJ.send({ev: "tx", phase: "done", name: tx.plan.name, from: tx.A, to: tx.B});
};

RobotDJ.abort = function(reason) {
    var tx = RobotDJ.tx;
    if (!tx) { return; }
    RobotDJ.cleanup(tx);
    RobotDJ.neutral(tx.A);
    if (tx.fired) { RobotDJ.setv(tx.B, "play", 0); }
    RobotDJ.neutral(tx.B);
    RobotDJ.tx = null;
    RobotDJ.send({ev: "tx", phase: "aborted", name: tx.plan.name, reason: reason});
};


// Loslaten: de mens neemt over. Timers en verbindingen stoppen, maar NIETS terugzetten —
// faders, EQ, sync en loops blijven precies zoals ze nu staan.
RobotDJ.release = function() {
    var tx = RobotDJ.tx;
    if (!tx) { return false; }
    if (tx.scr) { RobotDJ.setv(tx.scr.g, "scratch2", 0); RobotDJ.setv(tx.scr.g, "scratch2_enable", 0); tx.scr = null; }
    if (tx.timer) { engine.stopTimer(tx.timer); tx.timer = 0; }
    if (tx.beatConn) { tx.beatConn.disconnect(); tx.beatConn = null; }
    if (tx.playConn) { tx.playConn.disconnect(); tx.playConn = null; }
    if (tx.guard) { engine.stopTimer(tx.guard); tx.guard = 0; }
    tx.ramps = [];
    tx.phase = "released";
    RobotDJ.tx = null;
    RobotDJ.send({ev: "tx", phase: "released", name: tx.plan.name, from: tx.A, to: tx.B, beat: tx.beat});
    return true;
};

// Versnellen: de lopende choreografie in twee maten afmaken. De klok springt op de volgende
// beat naar (einde − 8 tellen); stappen die al voorbij zijn worden meteen uitgevoerd en
// rampen die nog liepen krijgen de resterende tijd (catch-up in runDue). Alleen als de klok
// al loopt (vanaf beat 0); daarvóór laat de Bot het uitmixpunt zelf opschuiven.
RobotDJ.hurry = function() {
    var tx = RobotDJ.tx;
    if (!tx || !tx.clock) { return false; }
    var b = (Date.now() - tx.t0) / tx.beatMs;
    var target = Math.max(b, tx.plan.end - 8);
    if (target <= b + 0.05) { return true; }
    var wait = (1 - (b - Math.floor(b))) * tx.beatMs;          // op de volgende beat
    engine.beginTimer(Math.max(20, Math.round(wait)), function() {
        if (RobotDJ.tx !== tx || !tx.clock) { return; }
        tx.t0 -= (target - Math.floor(b + 1)) * tx.beatMs;
        RobotDJ.send({ev: "tx", phase: "mark", mark: "hurry", beat: target});
    }, true);
    return true;
};
