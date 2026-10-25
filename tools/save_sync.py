#!/usr/bin/env python3
"""Copy MC3 profiles between this computer and a Switch running an FTP server.

  save_sync.py down --host <switch ip>[:port] [options]   Switch -> computer
  save_sync.py up   --host <switch ip>[:port] [options]   computer -> Switch

  The port defaults to 21; another goes after the IP, e.g. 192.168.1.20:5000.

  --game-id ID   title ID folder (default 54540079, MC3)
  --profile P    one profile, by folder (0075195616BF) or name (laurd);
                 default all
  --udata DIR    the local UDATA folder (default $MC3_GAME_DIR/UDATA)
  --remote DIR   the Switch's UDATA folder, from the card's root. Default:
                 where the game writes them, in its disc folder as the NRO
                 picks it: /switch/<MC3_SD_DIR>/game-<edition>/UDATA if that
                 disc folder is on the card, else .../game/UDATA
                 (<MC3_SD_DIR>: mcdub3-nx, GAME=remix mcdub3remix-nx;
                 <edition>: EDITION=, else the local disc's, e.g. eur for
                 game-remix-eur/)
  --user / --password
                 only for a server that asks for a login. Without them the
                 tool logs in anonymously, or not at all if the server
                 needs no login; a user without a password sends an empty one
  --list         only show what is on both sides
  --yes          copy without asking (it asks otherwise, showing the
                 direction and which profiles are created or overwritten)

The receiving side is copied to work-<game>/saves-backup/<time>-<direction>/ first,
so nothing is lost by syncing the wrong way. Profiles move as they are: the
port signs them the same way on every platform (tools/save_import.py).
"""
import argparse, ftplib, io, ipaddress, os, posixpath, re, shutil, socket, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def ftp_is_dir(ftp, path):
    here = ftp.pwd()
    try:
        ftp.cwd(path)
        ftp.cwd(here)
        return True
    except ftplib.error_perm:
        return False


def ftp_names(ftp, path):
    try:
        names = ftp.nlst(path)
    except ftplib.error_perm:            # an empty folder answers 550 on some servers
        return []
    # Servers answer with bare names or with full paths.
    return sorted({posixpath.basename(n.rstrip('/')) for n in names} - {'.', '..', ''})


def ftp_walk(ftp, path):
    """{relative path: None} for the files under path (on the Switch)."""
    out = {}
    for n in ftp_names(ftp, path):
        full = posixpath.join(path, n)
        if ftp_is_dir(ftp, full):
            for k in ftp_walk(ftp, full):
                out[posixpath.join(n, k)] = None
        else:
            out[n] = None
    return out


def ftp_makedirs(ftp, path):
    parts, cur = [p for p in path.split('/') if p], ''
    for p in parts:
        cur += '/' + p
        if not ftp_is_dir(ftp, cur):
            try:
                ftp.mkd(cur)
            except ftplib.error_perm:
                pass


class NeedsTLS(Exception):
    pass


def connect(cls, host, port):
    ftp = cls()
    ftp.connect(host, port, timeout=15)
    return ftp


def login(ftp, user, password):
    """A login only as far as the server wants one: the given user (an empty
    password if none), else anonymous, else none at all."""
    try:
        if user:
            ftp.login(user, password)
        else:
            ftp.login()                          # anonymous
        return
    except ftplib.error_perm as e:
        if 'AUTH' in str(e) and not isinstance(ftp, ftplib.FTP_TLS):
            raise NeedsTLS() from e
        if user:
            raise SystemExit(f'the FTP server refused the login for {user!r}: {e}')
        refused = e
    try:                                         # a server that wants no USER at all
        ftp.voidcmd('NOOP')
        ftp.pwd()
    except ftplib.Error:
        raise SystemExit(f'the FTP server wants a login ({refused}): '
                         f'give FTP_USER= and FTP_PASS= (make), or --user/--password')


def parse_host(spec):
    """(host, port) from IP, IP:PORT or a host name; a clear error otherwise."""
    spec = spec.strip()
    host, port = spec, '21'
    if spec.count(':') == 1:
        host, port = spec.split(':')
    if not port.isdigit() or not 0 < int(port) < 65536:
        raise SystemExit(f'not a port: {port!r} in {spec!r} (use IP or IP:PORT, e.g. 192.168.1.20:5000)')
    if re.fullmatch(r'[0-9.]+', host):
        try:
            ipaddress.IPv4Address(host)
        except ValueError:
            raise SystemExit(f'not an IP address: {host!r} (four numbers 0-255, e.g. 192.168.1.20)')
    elif not re.fullmatch(r'(?=.{1,253}$)([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.?)+', host):
        raise SystemExit(f'not an IP address or host name: {host!r} (e.g. 192.168.1.20)')
    return host, int(port)


def connect_or_explain(cls, host, port):
    try:
        return connect(cls, host, port)
    except socket.gaierror:
        raise SystemExit(f'unknown host {host!r}: check the address the Switch\'s FTP server shows')
    except ConnectionRefusedError:
        raise SystemExit(f'{host} answers, but nothing listens on port {port}: is the FTP server running on '
                         f'the Switch, and on which port? (21 by default; another as {host}:5000)')
    except (socket.timeout, TimeoutError):
        raise SystemExit(f'no answer from {host}:{port} within 15 s: a wrong IP, another network, or the '
                         f'Switch asleep or its FTP server closed')
    except OSError as e:
        hint = 'Same network as the Switch (not a guest network)?'
        if e.errno == getattr(__import__('errno'), 'EHOSTUNREACH', None) and sys.platform == 'darwin':
            # macOS 15+ blocks connections into the local network for apps
            # without the permission: "No route to host" on the same Wi-Fi.
            hint = ('On a Mac this is usually the Local Network permission: System Settings > '
                    'Privacy & Security > Local Network > allow the app you run make in '
                    '(Terminal, iTerm, VS Code, VSCodium: it is per app), then restart it. Otherwise: same network '
                    '(not a guest one), and does ping ' + host + ' answer?')
        raise SystemExit(f'cannot reach {host}:{port}: {e.strerror or e}. {hint}')


def profile_name(meta):
    """The name in a SaveMeta.xbx (UTF-16, "Name=<name>")."""
    try:
        text = meta.decode('utf-16')
    except UnicodeDecodeError:
        return ''
    m = re.search(r'Name=([^\r\n]*)', text)
    return m.group(1) if m else ''


def profiles_local(title_dir):
    out = {}
    if os.path.isdir(title_dir):
        for d in sorted(os.listdir(title_dir)):
            meta = os.path.join(title_dir, d, 'SaveMeta.xbx')
            if os.path.isfile(meta):
                out[d] = profile_name(open(meta, 'rb').read())
    return out


def profiles_remote(ftp, title_dir):
    out = {}
    if not ftp_is_dir(ftp, title_dir):
        return out
    for d in ftp_names(ftp, title_dir):
        full = posixpath.join(title_dir, d)
        if ftp_is_dir(ftp, full):
            buf = io.BytesIO()
            try:
                ftp.retrbinary('RETR ' + posixpath.join(full, 'SaveMeta.xbx'), buf.write)
            except ftplib.error_perm:
                continue
            out[d] = profile_name(buf.getvalue())
    return out


def resolve_profile(want, profiles, side):
    """The folder of the profile asked for, by folder or by name."""
    for folder, name in profiles.items():
        if want.lower() in (folder.lower(), name.lower()):
            return folder
    listing = ', '.join(f'{f} ({n})' if n else f for f, n in profiles.items()) or 'none'
    raise SystemExit(f'no profile {want!r} {side}. There: {listing}')


def confirm(a, local, remote, here, there, folder, names):
    """Show the direction and what each profile will undergo; ask unless --yes."""
    src, dst = (there, here) if a.direction == 'down' else (here, there)
    if not src:
        return True                      # the caller reports there is nothing to copy
    if a.direction == 'down':
        print(f'\nDirection: Switch {a.host}:{a.port}  ->  this computer')
        print(f'  from  {remote}\n  to    {local}')
    else:
        print(f'\nDirection: this computer  ->  Switch {a.host}:{a.port}')
        print(f'  from  {local}\n  to    {remote}')

    def top(files):
        return {k.split('/')[0] for k in files if folder or '/' in k}
    if folder:                           # paths are inside the one profile
        what = [('overwrite' if dst else 'create', folder)]
    else:
        what = [('overwrite' if p in top(dst) else 'create', p) for p in sorted(top(src))]
    for action, p in what:
        name = names.get(p)
        print(f'  {action:<9}  {p}' + (f' ({name})' if name else ''))
    print('  (nothing is deleted; the receiving side is backed up first)')
    if a.yes:
        return True
    if not sys.stdin.isatty():
        print('not asked (no terminal to answer in): add YES=1 (make) or --yes to copy')
        return False
    try:
        answer = input('Copy? Type y to continue [y/N]: ')
    except EOFError:
        return False
    return answer.strip().lower() in ('y', 'yes')


def local_walk(root):
    out = {}
    for top, _, files in os.walk(root):
        for f in files:
            rel = os.path.relpath(os.path.join(top, f), root).replace(os.sep, '/')
            out[rel] = None
    return out


def describe(side, files, profile=None):
    if profile:                          # one profile: its own files
        print(f'  {side}: profile {profile}, {len(files)} files' if files else f'  {side}: profile {profile} not there yet')
        return
    profiles = sorted({k.split('/')[0] for k in files if '/' in k})
    print(f'  {side}: {len(files)} files, {len(profiles)} profiles {" ".join(profiles)}'.rstrip())


def remote_udata_default(ftp, local_udata):
    """The Switch's UDATA as the NRO finds its disc (src/main.c):
    game-<edition>/ if that folder is on the card, else game/. The game
    writes its saves inside the disc folder it plays from."""
    app = '/switch/%s' % os.environ.get('MC3_SD_DIR', 'mcdub3-nx')
    edition = os.environ.get('EDITION', '')
    if not edition:
        # The local disc folder's edition: game-eur -> eur, game-remix-usa -> usa.
        disc = os.path.basename(os.path.dirname(os.path.abspath(local_udata)))
        prefix = os.environ.get('MC3_DISC_PREFIX', 'game')
        if disc.startswith(prefix + '-'):
            edition = disc[len(prefix) + 1:]
    tried = []
    if edition:
        d = posixpath.join(app, 'game-' + edition)
        if ftp_is_dir(ftp, d):
            return posixpath.join(d, 'UDATA')
        tried.append(d)
    if tried:
        print(f'  ({tried[0]} is not on the card: using {app}/game)')
    return posixpath.join(app, 'game', 'UDATA')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('direction', choices=['up', 'down'])
    ap.add_argument('--host', required=True)
    ap.add_argument('--user', default='')
    ap.add_argument('--password', default='')
    ap.add_argument('--game-id', default='54540079')
    ap.add_argument('--profile')
    ap.add_argument('--udata')
    ap.add_argument('--remote')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--yes', action='store_true')
    a = ap.parse_args()
    a.host, a.port = parse_host(a.host)

    udata = os.path.expanduser(a.udata) if a.udata else \
        os.path.join(os.environ.get('MC3_GAME_DIR') or os.path.join(REPO, 'game'), 'UDATA')
    local = os.path.join(udata, a.game_id)
    remote_udata = a.remote          # chosen once connected (the card decides)
    if a.direction == 'up' and not os.path.isdir(local):
        raise SystemExit(f'no saves here: {local} does not exist (UDATA= for another folder)')

    ftp = connect_or_explain(ftplib.FTP, a.host, a.port)
    try:
        login(ftp, a.user, a.password)
    except NeedsTLS:
        # A server that only takes encrypted logins (FTPS: "503 Use AUTH
        # first" -- FileZilla Server, for one). Switch servers do not.
        ftp.close()
        ftp = connect_or_explain(ftplib.FTP_TLS, a.host, a.port)
        login(ftp, a.user, a.password)
        ftp.prot_p()
        print('  (encrypted connection: FTPS)')

    if not remote_udata:
        remote_udata = remote_udata_default(ftp, udata)
    remote = posixpath.join(remote_udata, a.game_id)
    print(f'Switch {a.host}:{a.port}  {remote}\nhere          {local}')

    folder = None
    # The sending side's profiles, {folder: name}: to find one by name, and to
    # name them in the confirmation.
    names = profiles_remote(ftp, remote) if a.direction == 'down' else profiles_local(local)
    if a.profile:
        # Resolved on the sending side, by folder or by name.
        folder = resolve_profile(a.profile, names, 'on the Switch' if a.direction == 'down' else 'here')
        if folder != a.profile:
            print(f'  profile {a.profile!r}: folder {folder}')
        local, remote = os.path.join(local, folder), posixpath.join(remote, folder)

    there = ftp_walk(ftp, remote) if ftp_is_dir(ftp, remote) else {}
    here = local_walk(local) if os.path.isdir(local) else {}
    describe('Switch', there, folder)
    describe('here  ', here, folder)
    if a.list:
        ftp.quit()
        return 0

    if not confirm(a, local, remote, here, there, folder, names):
        ftp.quit()
        print('nothing copied')
        return 1

    stamp = time.strftime('%Y%m%d_%H%M%S')
    backup = os.path.join(os.environ.get('MC3_WORK') or os.path.join(REPO, 'work-dub'),
                          'saves-backup', f'{stamp}-before-sync-{a.direction}')
    if a.direction == 'down':
        if not there:
            raise SystemExit(f'no saves on the Switch in {remote}')
        if here:
            shutil.copytree(local, backup)
            print(f'  local saves copied to {backup}')
        for rel in sorted(there):
            dst = os.path.join(local, *rel.split('/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f:
                ftp.retrbinary('RETR ' + posixpath.join(remote, rel), f.write)
            print(f'  <- {rel}')
    else:
        if not here:
            raise SystemExit(f'no saves here in {local}')
        if there:
            for rel in sorted(there):              # the Switch's saves, kept here first
                dst = os.path.join(backup, *rel.split('/'))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, 'wb') as f:
                    ftp.retrbinary('RETR ' + posixpath.join(remote, rel), f.write)
            print(f'  the Switch\'s saves copied to {backup}')
        for rel in sorted(here):
            dst = posixpath.join(remote, rel)
            ftp_makedirs(ftp, posixpath.dirname(dst))
            with open(os.path.join(local, *rel.split('/')), 'rb') as f:
                ftp.storbinary('STOR ' + dst, f)
            print(f'  -> {rel}')
    ftp.quit()
    print(f'done: {len(there) if a.direction == "down" else len(here)} files '
          f'{"from" if a.direction == "down" else "to"} the Switch')
    return 0


if __name__ == '__main__':
    sys.exit(main())
