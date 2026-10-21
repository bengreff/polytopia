// Frida agent injected into the real Polytopia app. It hooks named il2cpp methods and
// reports each call: decoded arguments, the return value, and the native call chain,
// which the host symbolizes. With snapshot set on a hook, it also serializes the
// GameState before and after the call with the game's own GameState.Serialize.
//
// The host sends {hooks: [{assembly, ns, cls, method, argc, snapshot}]} once the
// runtime is up.

let lib = null;
const api = {};
const API = {
  domain_get: ['pointer', []],
  domain_get_assemblies: ['pointer', ['pointer', 'pointer']],
  assembly_get_image: ['pointer', ['pointer']],
  image_get_name: ['pointer', ['pointer']],
  class_from_name: ['pointer', ['pointer', 'pointer', 'pointer']],
  class_get_method_from_name: ['pointer', ['pointer', 'pointer', 'int']],
  class_get_name: ['pointer', ['pointer']],
  class_get_namespace: ['pointer', ['pointer']],
  class_get_fields: ['pointer', ['pointer', 'pointer']],
  class_get_parent: ['pointer', ['pointer']],
  class_is_enum: ['bool', ['pointer']],
  class_is_valuetype: ['bool', ['pointer']],
  class_value_size: ['int', ['pointer', 'pointer']],
  class_from_type: ['pointer', ['pointer']],
  field_get_name: ['pointer', ['pointer']],
  field_get_offset: ['size_t', ['pointer']],
  field_get_type: ['pointer', ['pointer']],
  field_get_flags: ['int', ['pointer']],
  class_get_field_from_name: ['pointer', ['pointer', 'pointer']],
  field_get_value: ['void', ['pointer', 'pointer', 'pointer']],
  type_get_type: ['int', ['pointer']],
  method_get_param_count: ['uint32', ['pointer']],
  method_get_name: ['pointer', ['pointer']],
  method_get_param: ['pointer', ['pointer', 'uint32']],
  method_get_return_type: ['pointer', ['pointer']],
  method_get_flags: ['uint32', ['pointer', 'pointer']],
  field_set_value: ['void', ['pointer', 'pointer', 'pointer']],
  object_get_class: ['pointer', ['pointer']],
  object_new: ['pointer', ['pointer']],
  object_unbox: ['pointer', ['pointer']],
  runtime_invoke: ['pointer', ['pointer', 'pointer', 'pointer', 'pointer']],
  string_chars: ['pointer', ['pointer']],
  string_length: ['int', ['pointer']],
  array_length: ['uint32', ['pointer']],
  thread_attach: ['pointer', ['pointer']],
  format_exception: ['void', ['pointer', 'pointer', 'int']],
  format_stack_trace: ['void', ['pointer', 'pointer', 'int']],
};

function bind() {
  for (const [name, [ret, args]] of Object.entries(API))
    api[name] = new NativeFunction(lib.getExportByName('il2cpp_' + name), ret, args);
}
const cstr = p => p.isNull() ? null : p.readUtf8String();

function image(name) {
  const n = Memory.alloc(8);
  const as = api.domain_get_assemblies(api.domain_get(), n);
  for (let i = 0; i < n.readU64(); i++) {
    const img = api.assembly_get_image(as.add(i * 8).readPointer());
    const iname = cstr(api.image_get_name(img));
    if (iname === name || iname === name + '.dll') return img;
  }
  return null;
}
function klass(assembly, ns, name) {
  const img = image(assembly);
  return img ? api.class_from_name(img, Memory.allocUtf8String(ns), Memory.allocUtf8String(name)) : ptr(0);
}
function methodOf(k, name, argc) { return api.class_get_method_from_name(k, Memory.allocUtf8String(name), argc); }

function str(p) {
  if (p.isNull()) return null;
  return p.add(0x14).readUtf16String(api.string_length(p));
}
const className = k => { const ns = cstr(api.class_get_namespace(k)); const n = cstr(api.class_get_name(k)); return ns ? `${ns}.${n}` : n; };

// Decodes a value of an il2cpp type found at addr. Objects are expanded to `depth` levels.
function value(type, addr, depth) {
  const kind = api.type_get_type(type);
  switch (kind) {
    case 0x02: return addr.readU8() !== 0;
    case 0x04: return addr.readS8();
    case 0x05: return addr.readU8();
    case 0x06: return addr.readS16();
    case 0x07: return addr.readU16();
    case 0x08: return addr.readS32();
    case 0x09: return addr.readU32();
    case 0x0a: return addr.readS64().toString();
    case 0x0b: return addr.readU64().toString();
    case 0x0c: return addr.readFloat();
    case 0x0d: return addr.readDouble();
    case 0x0e: return str(addr.readPointer());
    case 0x11: case 0x55: {
      const k = api.class_from_type(type);
      if (api.class_is_enum(k)) {
        const size = api.class_value_size(k, ptr(0));
        return size === 1 ? addr.readU8() : size === 2 ? addr.readU16() : addr.readS32();
      }
      return fields(k, addr.sub(0x10), depth);
    }
    case 0x12: case 0x15: case 0x1c: case 0x1d: case 0x14:
      return object(addr.readPointer(), depth);
    default: return `<type ${kind}>`;
  }
}
function fields(k, base, depth) {
  const out = { __class: className(k) };
  const chain = [];
  for (let c = k; !c.isNull(); c = api.class_get_parent(c)) {
    const n = cstr(api.class_get_name(c));
    if (n === 'Object' || n === 'ValueType') break;
    chain.unshift(c);
  }
  for (const c of chain) {
    const iter = Memory.alloc(8); iter.writePointer(ptr(0));
    for (let f; !(f = api.class_get_fields(c, iter)).isNull();) {
      if (api.field_get_flags(f) & 0x50) continue;  // static or const
      let name = cstr(api.field_get_name(f));
      const m = /^<(.+)>k__BackingField$/.exec(name); if (m) name = m[1];
      out[name] = value(api.field_get_type(f), base.add(api.field_get_offset(f)), depth - 1);
    }
  }
  return out;
}
function object(p, depth) {
  if (p.isNull()) return null;
  const k = api.object_get_class(p);
  const name = className(k);
  if (name === 'System.String') return str(p);
  if (depth <= 0) return { __class: name, __ptr: p.toString() };
  if (name.endsWith('[]')) {
    const n = api.array_length(p), items = [];
    for (let i = 0; i < Math.min(n, 64); i++) items.push(object(p.add(0x20 + i * 8).readPointer(), depth - 1));
    return { __class: name, length: n, items };
  }
  if (name.startsWith('System.Collections.Generic.List')) {
    const items = p.add(0x10).readPointer(), size = p.add(0x18).readS32();
    const out = { __class: name, count: size };
    if (!items.isNull()) {
      const rt = api.method_get_return_type(methodOf(k, 'get_Item', 1)), t = api.class_from_type(rt);
      const step = api.class_is_valuetype(t) ? api.class_value_size(t, ptr(0)) : 8;
      out.items = [];
      for (let i = 0; i < Math.min(size, 64); i++) out.items.push(value(rt, items.add(0x20 + i * step), depth - 1));
    }
    return out;
  }
  return fields(k, p, depth);
}

// Serializes a GameState with the game's own writer: new BinaryWriter(new MemoryStream()).
let ser = null, callSeq = 0;
function serialize(gameState) {
  if (!ser) {
    const ms = klass('mscorlib', 'System.IO', 'MemoryStream'), bw = klass('mscorlib', 'System.IO', 'BinaryWriter');
    const gs = klass('GameLogicAssembly', '', 'GameState');
    ser = { ms, bw, msCtor: methodOf(ms, '.ctor', 0), bwCtor: methodOf(bw, '.ctor', 1), toArray: methodOf(ms, 'ToArray', 0),
            serialize: methodOf(gs, 'Serialize', 2), version: methodOf(gs, 'get_Version', 0) };
  }
  const exc = Memory.alloc(8);
  const call = (m, self, args) => {
    exc.writePointer(ptr(0));
    const argv = Memory.alloc(8 * Math.max(1, args.length));
    args.forEach((a, i) => argv.add(i * 8).writePointer(a));
    const r = api.runtime_invoke(m, self, argv, exc);
    if (!exc.readPointer().isNull()) throw new Error('managed exception during serialize');
    return r;
  };
  const stream = api.object_new(ser.ms); call(ser.msCtor, stream, []);
  const writer = api.object_new(ser.bw); call(ser.bwCtor, writer, [stream]);
  const version = api.object_unbox(call(ser.version, gameState, [])).readS32();
  const v = Memory.alloc(4); v.writeS32(version);
  call(ser.serialize, gameState, [writer, v]);
  const arr = call(ser.toArray, stream, []);
  return arr.add(0x20).readByteArray(api.array_length(arr));
}

function install(hooks) {
  let installed = 0;
  for (const h of hooks) {
    const k = klass(h.assembly, h.ns || '', h.cls);
    if (k.isNull()) { send({ type: 'error', text: `no class ${h.cls}` }); continue; }
    const m = methodOf(k, h.method, h.argc);
    if (m.isNull()) { send({ type: 'error', text: `no method ${h.cls}.${h.method}/${h.argc}` }); continue; }
    const target = m.readPointer();
    const isStatic = (api.method_get_flags(m, ptr(0)) & 0x10) !== 0;
    const params = [];
    for (let i = 0; i < h.argc; i++) params.push(api.method_get_param(m, i));
    const label = `${h.cls}.${h.method}`;
    const depth = h.depth ?? 2;
    Interceptor.attach(target, {
      onEnter(args) {
        this.rec = { type: 'call', fn: label, tid: Process.getCurrentThreadId() };
        if (h.backtrace !== false)
          try { this.rec.bt = Thread.backtrace(this.context, Backtracer.FUZZY).slice(0, 24).map(a => a.sub(lib.base).toString()); } catch (e) {}
        try {
          const off = isStatic ? 0 : 1;
          if (!isStatic) this.rec.self = h.selfDepth ? object(args[0], h.selfDepth) : className(api.object_get_class(args[0]));
          this.rec.args = params.map((t, i) => {
            const kind = api.type_get_type(t);
            const raw = args[i + off];
            if (kind === 0x12 || kind === 0x15 || kind === 0x1c || kind === 0x1d || kind === 0x0e || kind === 0x14) return object(raw, depth);
            const slot = Memory.alloc(8); slot.writePointer(raw);
            return value(t, slot, depth);
          });
          this.self = isStatic ? null : args[0];
          this.args = []; for (let i = 0; i < h.argc + 1; i++) this.args.push(args[i]);
        } catch (e) { this.rec.decodeError = String(e); }
      },
      onLeave(ret) {
        try {
          const rt = api.method_get_return_type(m), kind = api.type_get_type(rt);
          if (kind !== 0x01) {
            if (kind === 0x12 || kind === 0x15 || kind === 0x1c || kind === 0x1d || kind === 0x0e) this.rec.ret = object(ret, h.retDepth ?? 1);
            else { const slot = Memory.alloc(8); slot.writePointer(ret); this.rec.ret = value(rt, slot, 1); }
          }
        } catch (e) { this.rec.retError = String(e); }
        const seq = ++callSeq;
        this.rec.callSeq = seq;
        send(this.rec);
        for (const snap of h.snapshots || []) {
          try {
            let state = null;
            if (snap.arg) state = this.args[(isStatic ? 0 : 1) + snap.arg - 1];
            else if (snap.ret) state = ret;
            else if (snap.field) {
              const f = api.class_get_field_from_name(api.object_get_class(this.self), Memory.allocUtf8String(snap.field));
              const slot = Memory.alloc(8); api.field_get_value(this.self, f, slot); state = slot.readPointer();
            }
            if (state && !state.isNull()) send({ type: 'snapshot', callSeq: seq, fn: label, source: snap }, serialize(state));
          } catch (e) { send({ type: 'snapshot-error', callSeq: seq, fn: label, error: String(e) }); }
        }
      },
    });
    installed++;
  }
  send({ type: 'ready', installed });
}

function waitForRuntime() {
  lib = Process.findModuleByName('libil2cpp.so');
  if (!lib) { setTimeout(waitForRuntime, 100); return; }
  bind();
  // Calling into il2cpp before il2cpp_init has finished faults, so wait for init to
  // return (or for the runtime to answer, when attached late), retrying on faults.
  let initDone = false;
  Interceptor.attach(lib.getExportByName('il2cpp_init'), { onLeave() { initDone = true; } });
  const ready = () => {
    try {
      return !api.domain_get().isNull() && image('GameLogicAssembly') && !klass('GameLogicAssembly', '', 'GameState').isNull();
    } catch (e) { return false; }
  };
  const poll = () => {
    // Querying il2cpp before il2cpp_init returns can abort the process, so wait for init
    // when we got here first; when attached to a running app (init long done), go ahead.
    if (!(initDone || '__attached' in globalThis) || !ready()) { setTimeout(poll, 100); return; }
    api.thread_attach(api.domain_get());
    send({ type: 'runtime', base: lib.base.toString() });
    recv('hooks', msg => install(msg.hooks));
  };
  poll();
}
if (Process.findModuleByName('libil2cpp.so')) globalThis.__attached = true;  // attached to a running app
waitForRuntime();

// ---------- autoplay: the real app plays the local seat with its own AI ----------
// The host walks the menus with taps to the Game Setup screen, then posts
// {type:'configure', game:{mode, rulesMode, mapPreset, mapSize, opponents, difficulty, turns}};
// the agent writes those into GameManager's settings on the main thread and replies
// 'configured', and the host taps Start. From then on, each frame (GameManager.Update) while the
// local player is to move and the client is idle, it plays CommandTriggerUtils.TryGetTriggerCommand
// or AI.GetMove for the local player through ClientBase.SendCommand, exactly as a tap would, and
// reports 'autoplay-end' when the game ends or reaches the turn cap.
function invoke(m, self, args = []) {
  const exc = Memory.alloc(8); exc.writePointer(ptr(0));
  const argv = Memory.alloc(8 * Math.max(1, args.length));
  args.forEach((a, i) => argv.add(i * 8).writePointer(a));
  const r = api.runtime_invoke(m, self, argv, exc);
  if (!exc.readPointer().isNull()) {
    const buf = Memory.alloc(2048);
    api.format_exception(exc.readPointer(), buf, 2048);
    throw new Error(`managed exception in ${cstr(api.method_get_name(m))}: ${buf.readUtf8String()}`);
  }
  return r;
}
const i32 = v => { const p = Memory.alloc(8); p.writeS32(v); return p; };
const unboxU8 = o => api.object_unbox(o).readU8();
const unboxBool = o => api.object_unbox(o).readU8() !== 0;

function startAutoplay() {
  const P = 'PolytopiaAssembly', G = 'GameLogicAssembly';
  const gmClass = klass(P, '', 'GameManager'), cbClass = klass(P, '', 'ClientBase'), camClass = klass(P, '', 'ClientActionManager');
  const gsClass = klass(G, '', 'GameState'), psClass = klass(G, '', 'PlayerState'), setClass = klass(G, '', 'GameSettings');
  const M = {
    client: methodOf(gmClass, 'get_Client', 0), instance: methodOf(gmClass, 'get_Instance', 0),
    gameState: methodOf(cbClass, 'get_GameState', 0), local: methodOf(cbClass, 'GetCurrentLocalPlayer', 0),
    send: methodOf(cbClass, 'SendCommand', 1), processing: methodOf(camClass, 'get_IsProcessing', 0),
    current: methodOf(gsClass, 'get_CurrentPlayer', 0), state: methodOf(gsClass, 'get_CurrentState', 0),
    turn: methodOf(gsClass, 'get_CurrentTurn', 0), id: methodOf(psClass, 'get_Id', 0),
    trigger: methodOf(klass(G, '', 'CommandTriggerUtils'), 'TryGetTriggerCommand', 2),
    getMove: methodOf(klass(G, '', 'AI'), 'GetMove', 3),
    baseMode: methodOf(setClass, 'set_BaseGameMode', 1), rulesMode: methodOf(setClass, 'set_RulesGameMode', 1),
    opponents: methodOf(setClass, 'set_OpponentCount', 1), difficulty: methodOf(setClass, 'set_Difficulty', 1),
    mapSize: methodOf(setClass, 'set_MapSize', 1), loadPreset: methodOf(klass(G, '', 'GameRules'), 'LoadPreset', 1),
  };
  const mapPresetField = api.class_get_field_from_name(setClass, Memory.allocUtf8String('mapPreset'));
  const rulesField = api.class_get_field_from_name(setClass, Memory.allocUtf8String('rules'));
  let pending = null, game = null, frame = 0, lastIssued = -1, commands = 0, ended = false;
  let queue = [], createAt = -1, index = -1, endedAt = -1;
  const create = methodOf(gmClass, 'CreateSinglePlayerGame', 0);
  const killAll = methodOf(klass('DOTween', 'DG.Tweening', 'DOTween'), 'KillAll', 1);
  const setTribe = methodOf(gmClass, 'set_StartingTribe', 1), setClimate = methodOf(gmClass, 'set_StartingClimate', 1);
  const setSkin = methodOf(gmClass, 'set_StartingSkin', 1), setMix = methodOf(gmClass, 'set_StartingTribeMix', 1);
  function nextGame() {
    if (!queue.length) { game = null; send({ type: 'autoplay-done' }); return; }
    const g = queue.shift(); index++;
    configure(g);
    invoke(setTribe, ptr(0), [i32(g.tribe)]); invoke(setClimate, ptr(0), [i32(g.tribe)]);
    invoke(setSkin, ptr(0), [i32(0)]); invoke(setMix, ptr(0), [i32(0)]);
    const f = Memory.alloc(8); f.writeU8(0); invoke(killAll, ptr(0), [f]);
    ended = false; commands = 0;
    send({ type: 'autoplay-start', index, game: g });
    invoke(create, invoke(M.instance, ptr(0)));
    createAt = frame;
  }

  function configure(g) {
    const settings = invoke(M.instance, ptr(0)).add(0x20).readPointer();  // GameManager.settings
    const rulesMode = g.rulesMode ?? g.mode;
    invoke(M.baseMode, settings, [i32(g.mode)]);
    invoke(M.rulesMode, settings, [i32(rulesMode)]);
    const rules = Memory.alloc(8); api.field_get_value(settings, rulesField, rules);
    invoke(M.loadPreset, rules.readPointer(), [i32(rulesMode)]);
    invoke(M.opponents, settings, [i32(g.opponents)]);
    invoke(M.difficulty, settings, [i32(g.difficulty)]);
    invoke(M.mapSize, settings, [i32(g.mapSize)]);
    const mp = Memory.alloc(4); mp.writeS32(g.mapPreset); api.field_set_value(settings, mapPresetField, mp);
    game = g;
    send({ type: 'configured', game: g });
  }

  function step() {
    frame++;
    if (pending) { const g = pending; pending = null; configure(g); return; }
    if (queue.length && !game) { nextGame(); return; }
    if (!game) return;
    if (ended) { if (frame - endedAt > 60) nextGame(); return; }
    const client = invoke(M.client, ptr(0));
    if (client.isNull()) return;
    // ClientActionManager.isSimulating from the first frame the client exists: actions are
    // processed without animations or UI reactions (popups such as the intro), as in replays.
    const am0 = client.add(0x18).readPointer();
    if (!am0.isNull() && game.simulate === true) am0.add(0x6c).writeU8(1);
    if (frame - createAt < 60) return;
    if (frame % 300 === 0) {
      const g0 = invoke(M.gameState, client), a0 = client.add(0x18).readPointer();
      send({ type: 'autoplay-status', frame, processing: a0.isNull() ? null : unboxBool(invoke(M.processing, a0)),
             simulating: a0.isNull() ? null : a0.add(0x6c).readU8(), paused: a0.isNull() ? null : a0.add(0x69).readU8(),
             holding: a0.isNull() ? null : a0.add(0x6d).readU8(), current: g0.isNull() ? null : unboxU8(invoke(M.current, g0)) });
    }
    const gs = invoke(M.gameState, client);
    if (gs.isNull()) return;
    const am = client.add(0x18).readPointer();  // ClientBase.ActionManager
    if (am.isNull()) return;
    // ClientActionManager.isSimulating: process actions without animations or UI reactions
    // (popups), as replays do. The rules executed are the same; the UI is skipped.
    if (game.simulate === true) am.add(0x6c).writeU8(1);
    if (unboxBool(invoke(M.processing, am))) return;
    const state = unboxU8(invoke(M.state, gs)), turn = api.object_unbox(invoke(M.turn, gs)).readU32();
    if (state === 4 || turn >= (game.turns ?? 40)) {
      ended = true; endedAt = frame;
      send({ type: 'autoplay-end', index, state, turn, commands });
      return;
    }
    const local = invoke(M.local, client);
    if (local.isNull() || unboxU8(invoke(M.current, gs)) !== unboxU8(invoke(M.id, local))) return;
    if (frame - lastIssued < 3) return;
    const out = Memory.alloc(8); out.writePointer(ptr(0));
    let cmd = unboxBool(invoke(M.trigger, ptr(0), [gs, out])) ? out.readPointer() : ptr(0);
    if (cmd.isNull()) cmd = invoke(M.getMove, ptr(0), [gs, local, i32(0)]);
    if (cmd.isNull()) return;
    invoke(M.send, client, [cmd]);
    commands++;
    lastIssued = frame;
  }

  // Popups pause the client's action processing until dismissed (ClientActionManager.Pause,
  // HoldExecution). Make those no-ops so the game never waits on UI; popups still show.
  for (const [name, argc] of [['Pause', 0], ['HoldExecution', 1]]) {
    const target = methodOf(camClass, name, argc).readPointer();
    Interceptor.replace(target, new NativeCallback(() => {}, 'void', argc ? ['pointer', 'bool', 'pointer'] : ['pointer', 'pointer']));
  }
  const update = methodOf(gmClass, 'Update', 0).readPointer();
  Interceptor.attach(update, { onLeave() { try { step(); } catch (e) { send({ type: 'autoplay-error', error: String(e) }); } } });
  const wait = () => recv('configure', msg => { pending = msg.game; wait(); });
  wait();
  const waitGames = () => recv('games', msg => { queue = queue.concat(msg.games); waitGames(); });
  waitGames();
  send({ type: 'autoplay-ready' });
}
recv('autoplay', () => startAutoplay());

// {type:'create'}: call GameManager.CreateSinglePlayerGame on the main thread with DOTween killed
// first (as GameSetupScreen.OnStartGameClickedAndFaded does), reporting where any
// NullReferenceException is raised (the runtime's null-check failure function).
let createRequested = false, nreWatch = false;
function setupCreate() {
  const P = 'PolytopiaAssembly';
  const gmClass = klass(P, '', 'GameManager');
  const create = methodOf(gmClass, 'CreateSinglePlayerGame', 0), instance = methodOf(gmClass, 'get_Instance', 0);
  const killAll = methodOf(klass('DOTween', 'DG.Tweening', 'DOTween'), 'KillAll', 1);
  Interceptor.attach(lib.base.add(0x25d6600), { onEnter() {
    if (nreWatch) send({ type: 'call', fn: 'NullReferenceException', bt: Thread.backtrace(this.context, Backtracer.FUZZY).slice(0, 16).map(a => a.sub(lib.base).toString()) });
  } });
  Interceptor.attach(methodOf(gmClass, 'Update', 0).readPointer(), { onLeave() {
    if (!createRequested) return;
    createRequested = false;
    nreWatch = true;
    try {
      if (!killAll.isNull()) { const f = Memory.alloc(8); f.writeU8(0); invoke(killAll, ptr(0), [f]); }
      invoke(create, invoke(instance, ptr(0)));
      send({ type: 'created' });
    } catch (e) { send({ type: 'create-error', error: String(e) }); }
    nreWatch = false;
  } });
  const wait = () => recv('create', () => { createRequested = true; wait(); });
  wait();
}
recv('setup-create', () => setupCreate());
