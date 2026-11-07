# -*- coding: utf-8 -*-
"""验证 1.1.2 失败应答：错误令牌 + 故障通道（不会真的开泵）。"""
import socket, ssl, base64, os, struct, json, time, urllib.request

HOST, PORT, PATH = 'bemfa.com', 9504, '/wss'
UID = 'c90294a7be154afb80b9d63900da8f4a'
STATE_TOPIC, CMD_TOPIC = 'awAF48state', 'awAF48ctrl'

cfg = json.loads(urllib.request.urlopen('http://192.168.31.177/api/mqtt', timeout=5).read())
tok = cfg.get('token', '') or ''
print('device token_len=%s' % len(tok), flush=True)


def enc_len(n):
    out = b''
    while True:
        d = n % 128
        n //= 128
        if n:
            d |= 0x80
        out += bytes([d])
        if not n:
            return out


def lp(x):
    b = x.encode() if isinstance(x, str) else x
    return len(b).to_bytes(2, 'big') + b


def connect_pkt():
    vh = lp('MQTT') + bytes([4, 0xC2]) + (60).to_bytes(2, 'big')
    body = vh + lp(UID) * 3
    return bytes([0x10]) + enc_len(len(body)) + body


def subscribe_pkt():
    body = (1).to_bytes(2, 'big') + lp(STATE_TOPIC) + bytes([0])
    return bytes([0x82]) + enc_len(len(body)) + body


def pub(topic, obj):
    payload = json.dumps(obj, ensure_ascii=False, separators=(',', ':')).encode()
    body = lp(topic) + payload
    return bytes([0x30]) + enc_len(len(body)) + body


def ws_send(s, payload):
    mask = os.urandom(4)
    n = len(payload)
    hdr = bytearray([0x82])
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126)
        hdr += struct.pack('>H', n)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack('>Q', n)
    hdr += mask
    s.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def ws_frames(buf):
    out = []
    while len(buf) >= 2:
        fin, op = buf[0] >> 7, buf[0] & 15
        masked, ln = buf[1] >> 7, buf[1] & 127
        p = 2
        if ln == 126:
            if len(buf) < 4:
                break
            ln, p = int.from_bytes(buf[2:4], 'big'), 4
        elif ln == 127:
            if len(buf) < 10:
                break
            ln, p = int.from_bytes(buf[2:10], 'big'), 10
        mask = bytes(buf[p:p + 4]) if masked else None
        if mask:
            p += 4
        if len(buf) < p + ln:
            break
        data = bytes(buf[p:p + ln])
        if mask:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        del buf[:p + ln]
        if fin and op in (1, 2):
            out.append(data)
    return out


def mqtt_varint(buf):
    mul, val, i = 1, 0, 1
    while i < len(buf):
        b = buf[i]
        i += 1
        val += (b & 127) * mul
        if not (b & 0x80):
            return val, i
        mul *= 128
    return None


ctx = ssl.create_default_context()
s = ctx.wrap_socket(socket.create_connection((HOST, PORT), timeout=8), server_hostname=HOST)
s.settimeout(1.0)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n'
           'Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n'
           'Sec-WebSocket-Version: 13\r\n\r\n' % (PATH, HOST, PORT, key)).encode())
resp = b''
while b'\r\n\r\n' not in resp:
    resp += s.recv(4096)
print('WS', resp.split(b'\r\n')[0].decode(), flush=True)
wsbuf = bytearray(resp.split(b'\r\n\r\n', 1)[1])
ws_send(s, connect_pkt() + subscribe_pkt())

plan = [
    (1.0, 'WRONG_TOKEN', {'a': 'get', 'w': 'state', 't': 'WRONG_TOKEN_X'}),
    (5.0, 'FAULT_CH2', {'a': 'start', 'ch': 1, 's': 5, 't': tok}),
    (9.0, 'GOOD_GET', {'a': 'get', 'w': 'state', 't': tok}),
]
sent = set()
t0 = None
mqbuf = bytearray()
got = {'token': False, 'sensor_fault': False, 'state': False}
deadline = time.time() + 60
last_ping = time.time()
while time.time() < deadline:
    if time.time() - last_ping > 25:
        ws_send(s, bytes([0xC0, 0x00]))
        last_ping = time.time()
    try:
        d = s.recv(8192)
        if not d:
            break
        wsbuf.extend(d)
    except socket.timeout:
        pass
    except OSError as e:
        print('recv err', e, flush=True)
        break
    for frame in ws_frames(wsbuf):
        mqbuf.extend(frame)
        while len(mqbuf) >= 2:
            v = mqtt_varint(mqbuf)
            if v is None:
                break
            rem, hlen = v
            if len(mqbuf) < hlen + rem:
                break
            pkt = bytes(mqbuf[:hlen + rem])
            del mqbuf[:hlen + rem]
            typ, body = pkt[0] >> 4, pkt[hlen:]
            if typ == 9 and t0 is None:
                t0 = time.time()
                print('SUBACK ok, starting command sequence', flush=True)
            elif typ == 3:
                tl = int.from_bytes(body[:2], 'big')
                p = 2 + tl + (2 if (pkt[0] >> 1) & 3 else 0)
                try:
                    j = json.loads(body[p:].decode('utf-8'))
                    k = j.get('k', '?')
                    el = ('%.1fs' % (time.time() - t0)) if t0 else '?'
                    if k == 'e':
                        print('ERROR FRAME @%s err=%s ch=%s act=%s' % (
                            el, j.get('err'), j.get('ch'), j.get('act')), flush=True)
                        if j.get('err') == 'token':
                            got['token'] = True
                        if j.get('err') == 'sensor_fault' and j.get('ch') == 1:
                            got['sensor_fault'] = True
                    elif k == 's':
                        print('STATE FRAME @%s fw=%s' % (el, j.get('fw')), flush=True)
                        if t0 and time.time() - t0 > 8:
                            got['state'] = True
                except Exception as e:
                    print('json fail', e, flush=True)
    if t0:
        for off, tag, obj in plan:
            if tag not in sent and time.time() - t0 >= off:
                ws_send(s, pub(CMD_TOPIC, obj))
                sent.add(tag)
                print('SENT %s' % tag, flush=True)
    if t0 and time.time() - t0 > 16:
        break

print('RESULT:', got, 'PASS' if all(got.values()) else 'INCOMPLETE', flush=True)
s.close()
