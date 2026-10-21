// Records every call the client (PolytopiaAssembly) makes into the rules code
// (GameLogicAssembly), in order. Each rules method gets a native (CModule) hook that looks up
// the caller's method by binary search over all method start addresses and logs
// (rules method, caller method) when the caller belongs to the client. The host passes the
// tables: {starts: [rva...], owner: [assembly id...], hooks: [[index, rva]...], client: id}.

let lib = null;

const SOURCE = `
#include <gum/guminterceptor.h>
#include <glib.h>

extern const guint64 * starts;
extern const guint8 * owner;
extern const guint32 count;
extern const guint8 client;
extern const guint64 base;
extern guint32 * ring;
extern volatile gint head;
extern volatile gint hits;
extern volatile gint lastOwner;
extern guint64 * dbg;
#define RING_MASK ((1u << 20) - 1)

void on_enter(GumInvocationContext * ic) {
  guint64 ra = (guint64) ic->cpu_context->lr - base;  // the caller: a native probe has no return address
  guint32 lo = 0, hi = count;
  while (hi - lo > 1) { guint32 mid = (lo + hi) / 2; if (starts[mid] <= ra) lo = mid; else hi = mid; }
  gint k = g_atomic_int_add(&hits, 1);
  if ((k & 0xFFFF) == 0 && (k >> 16) < 256) { dbg[(k >> 16) * 2] = ra; dbg[(k >> 16) * 2 + 1] = lo; }
  lastOwner = owner[lo];
  if (owner[lo] != client) return;
  guint32 fn = (guint32) GPOINTER_TO_SIZE(gum_invocation_context_get_listener_function_data(ic));
  guint32 at = (guint32) g_atomic_int_add(&head, 2);
  ring[at & RING_MASK] = fn;
  ring[(at + 1) & RING_MASK] = lo;
}
`;

function install(msg) {
  const n = msg.starts.length;
  const starts = Memory.alloc(8 * n), owner = Memory.alloc(n);
  msg.starts.forEach((v, i) => starts.add(8 * i).writeU64(uint64(v)));
  owner.writeByteArray(msg.owner);
  const count = Memory.alloc(4); count.writeU32(n);
  const client = Memory.alloc(1); client.writeU8(msg.client);
  const base = Memory.alloc(8); base.writeU64(uint64(lib.base.toString()));
  const ring = Memory.alloc(4 << 20);
  const head = Memory.alloc(4); head.writeU32(0);
  const sp = Memory.alloc(8); sp.writePointer(starts);
  const op = Memory.alloc(8); op.writePointer(owner);
  const rp = Memory.alloc(8); rp.writePointer(ring);
  const hits = Memory.alloc(4); hits.writeU32(0);
  const lastOwner = Memory.alloc(4); lastOwner.writeU32(999);
  const dbgBuf = Memory.alloc(8 * 512); const dbgp = Memory.alloc(8); dbgp.writePointer(dbgBuf);
  const cm = new CModule(SOURCE, { starts: sp, owner: op, count, client, base, ring: rp, head, hits, lastOwner, dbg: dbgp });
  // The hooks call into cm and read these buffers for the life of the process.
  globalThis.keep = [cm, starts, owner, count, client, base, ring, head, sp, op, rp, hits, lastOwner, dbgBuf, dbgp];
  setInterval(() => {
    const samples = [];
    for (let i = 0; i < 16; i++) samples.push([dbgBuf.add(16 * i).readU64().toString(16), dbgBuf.add(16 * i + 8).readU64().toNumber()]);
    send({ type: 'stats', hits: hits.readU32(), head: head.readU32(), lastOwner: lastOwner.readU32(), samples });
  }, 5000);
  let ok = 0, failed = 0;
  for (const [index, rva] of msg.hooks) {
    try { Interceptor.attach(lib.base.add(rva), cm.on_enter, ptr(index)); ok++; } catch (e) { failed++; }
  }
  Interceptor.flush();
  send({ type: 'ready', hooked: ok, failed });
  let tail = 0;
  setInterval(() => {
    const h = head.readU32();
    if (h === tail) return;
    const out = [];
    for (; tail !== h; tail = (tail + 2) >>> 0) out.push(ring.add(4 * (tail & ((1 << 20) - 1))).readU32(), ring.add(4 * ((tail + 1) & ((1 << 20) - 1))).readU32());
    send({ type: 'calls', n: out.length / 2 }, new Uint32Array(out).buffer);
  }, 100);
}

function wait() {
  lib = Process.findModuleByName('libil2cpp.so');
  if (!lib) { setTimeout(wait, 100); return; }
  send({ type: 'loaded', base: lib.base.toString() });
  recv('tables', install);
}
wait();
