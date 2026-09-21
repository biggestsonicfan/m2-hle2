import assert from 'node:assert/strict';
import test from 'node:test';

import {
  AddressPool, Bucket, SIGNALING_TAG, ipv4, ipv4Text, isPublicUnicast,
  normalizeFingerprint, parseCidr, rewriteSignaling, route,
} from '../rules.mjs';

const pool = parseCidr('100.64.0.0/16');

test('ipv4 round trip', () => {
  assert.equal(ipv4('143.198.49.181'), 0x8FC631B5);
  assert.equal(ipv4Text(0x8FC631B5), '143.198.49.181');
  assert.throws(() => ipv4('1.2.3'));
  assert.throws(() => ipv4('1.2.3.256'));
});

test('public unicast', () => {
  for (const a of ['8.8.8.8', '143.198.49.181', '1.1.1.1', '100.63.255.255', '100.128.0.1']) assert.ok(isPublicUnicast(ipv4(a)), a);
  for (const a of ['127.0.0.1', '10.1.2.3', '192.168.1.1', '172.16.0.1', '172.31.255.255', '169.254.1.1',
                   '100.64.0.1', '0.0.0.0', '224.0.0.1', '255.255.255.255', '198.18.0.1']) {
    assert.ok(!isPublicUnicast(ipv4(a)), a);
  }
});

test('route: ping, signaling, pool, udp, refusals', () => {
  const ctx = { pool, self: ipv4('143.198.49.181') };
  assert.deepEqual(route(0, 0, ctx), { kind: 'ping' });
  assert.deepEqual(route(SIGNALING_TAG, 3657, ctx), { kind: 'signaling' });
  assert.equal(route(SIGNALING_TAG, 3658, ctx).kind, 'refuse');
  assert.deepEqual(route(ipv4('100.64.0.7'), 3658, ctx), { kind: 'pool', vip: ipv4('100.64.0.7') });
  assert.equal(route(ipv4('100.64.0.7'), 3659, ctx).kind, 'refuse');
  assert.deepEqual(route(ipv4('8.8.8.8'), 3658, ctx), { kind: 'udp' });
  assert.equal(route(ipv4('8.8.8.8'), 53, ctx).kind, 'refuse');
  assert.equal(route(ipv4('127.0.0.1'), 31313, ctx).kind, 'refuse');
  assert.equal(route(ipv4('192.168.1.10'), 3658, ctx).kind, 'refuse');
  assert.equal(route(ipv4('143.198.49.181'), 31313, ctx).kind, 'refuse');
  assert.deepEqual(route(ipv4('192.168.1.10'), 3658, { ...ctx, allowPrivate: true }), { kind: 'udp' });
});

test('signaling rewrite puts the virtual address in bytes 9..12', () => {
  const pkt = Buffer.from([1, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0]);
  const out = rewriteSignaling(pkt, ipv4('100.64.1.2'));
  assert.deepEqual([...out.subarray(9)], [100, 64, 1, 2]);
  assert.deepEqual([...out.subarray(0, 9)], [...pkt.subarray(0, 9)]);
  assert.equal(pkt[9], 0, 'the original is not modified');
  assert.equal(rewriteSignaling(Buffer.alloc(12, 1), 1), null);
  assert.equal(rewriteSignaling(Buffer.alloc(13, 2), 1), null);
});

test('address pool', () => {
  const p = new AddressPool('10.0.0.0/30');   /* two usable */
  const a = p.take(), b = p.take();
  assert.deepEqual([ipv4Text(a), ipv4Text(b)], ['10.0.0.1', '10.0.0.2']);
  assert.equal(p.take(), null);
  p.release(a);
  assert.equal(p.take(), a);
});

test('bucket', () => {
  const b = new Bucket(2, 0);
  assert.ok(b.take(1, 0));
  assert.ok(b.take(1, 0));
  assert.ok(!b.take(1, 0));
  assert.ok(b.take(1, 600));
});

test('fingerprint normalisation', () => {
  const hex = 'ab'.repeat(32);
  assert.equal(normalizeFingerprint(hex.match(/../g).join(':')), 'AB'.repeat(32));
  assert.equal(normalizeFingerprint('abc'), '');
});
