#!/usr/bin/env python3
"""Enable Clash TUN over the active AIC link, including legacy-route migration."""
import argparse
import http.client
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time


def command(*args):
    return subprocess.check_output(args, text=True)


def aic_interface():
    listing = subprocess.check_output(
        ['/usr/sbin/scutil'], input='list State:/Network/Service/AIC8800-.*/IPv4\n', text=True)
    interfaces = set()
    for key in re.findall(r'State:/Network/Service/AIC8800-[^\s]+/IPv4', listing):
        state = subprocess.check_output(
            ['/usr/sbin/scutil'], input='show ' + key + '\n', text=True)
        match = re.search(r'InterfaceName\s*:\s*(utun\d+)\b', state)
        if match:
            interfaces.add(match[1])
    if len(interfaces) != 1:
        raise RuntimeError('需要一个已联网的 AIC 接口；请先运行 connect.command 并等待联网验证成功。')
    return interfaces.pop()


class Controller(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost', timeout=10)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)

    def api(self, method, endpoint, value=None):
        self.request(method, endpoint, None if value is None else json.dumps(value),
                     {'Content-Type': 'application/json'})
        response = self.getresponse()
        data = response.read()
        if response.status >= 300:
            raise RuntimeError('Clash API: HTTP %s %s' % (response.status, data.decode()))
        return json.loads(data) if data else None


def legacy_routes(interface):
    routes = []
    for line in command('/usr/sbin/netstat', '-rn', '-f', 'inet').splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[3] == interface and 'I' not in fields[2]:
            if fields[0] in ('0/1', '0.0.0.0/1'):
                routes.append('0.0.0.0/1')
            elif fields[0] in ('128.0/1', '128.0.0.0/1'):
                routes.append('128.0.0.0/1')
    return routes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', help='Clash 控制 Unix socket（默认自动查找当前用户服务）')
    args = parser.parse_args()
    interface = aic_interface()
    uid = os.environ.get('SUDO_UID', str(os.getuid()))
    path = args.socket or '/var/run/clash-verge-service/users/%s/verge-mihomo.sock' % uid
    if not Path(path).is_socket():
        raise RuntimeError('未找到 Clash 控制接口；请先启动 Clash Verge 服务模式，或使用 --socket。')
    api = Controller(path)
    previous = api.api('GET', '/configs')
    stale = legacy_routes(interface)
    if stale and os.geteuid() != 0:
        raise RuntimeError('旧驱动仍占有 /1 路由；请用 sudo 再运行本脚本以迁移当前连接。')
    # Only exact prefixes still owned by AIC are removed. configd's ordinary
    # default and the interface-scoped default continue carrying the link.
    for network in stale:
        if network in legacy_routes(interface):
            subprocess.run(['/sbin/route', '-n', 'delete', '-net', network,
                            '-interface', interface], check=True)
    # The ordinary AIC default is sufficient for Mihomo's interface monitor.
    # Avoid pinning an ephemeral utun number or changing the user's stack.
    patch = {'interface-name': '',
             'tun': {'enable': True, 'auto-route': True,
                     'auto-detect-interface': True}}
    api.api('PATCH', '/configs', patch)
    # A successful PATCH can precede a failed asynchronous TUN startup.
    for _ in range(30):
        time.sleep(0.2)
        current = api.api('GET', '/configs')
        tunnel = current.get('tun', {})
        if tunnel.get('enable') and tunnel.get('device'):
            print('Clash TUN %s 已启用，AIC 出口 %s。' % (tunnel['device'], interface))
            print('正在验证普通 HTTPS（无代理参数）……', flush=True)
            subprocess.run(['/usr/bin/curl', '--disable', '--noproxy', '*', '--ipv4',
                            '--connect-timeout', '10', '--max-time', '30', '--silent',
                            '--show-error', '--fail', '--output', '/dev/null',
                            '--write-out', 'Google HTTPS: HTTP %{http_code}, local %{local_ip}, remote %{remote_ip}\\n',
                            'https://www.google.com/generate_204'], check=True)
            return
    api.api('PATCH', '/configs', {'interface-name': previous.get('interface-name', ''),
                                 'tun': previous.get('tun', {})})
    raise RuntimeError('Clash TUN 未成功建立，已恢复原核心配置；请检查核心日志。')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, http.client.HTTPException) as error:
        sys.exit(str(error))
