#!/usr/bin/env python3
"""Relay visible OMMesh map tiles from an authorized HTTPS provider to a trusted LAN."""

import argparse
import errno
import http.client
import ipaddress
import json
import re
import socket
import ssl
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer


OSM_TEMPLATE = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
USER_AGENT = "OMMeshBelleTileRelay/1.1 (+https://github.com/cm4ker/ommesh)"
DEFAULT_PORT = 8765
MAX_TILE_BYTES = 160 * 1024
TIMEOUT = 8
TILE_PATH = re.compile(r"/tiles/([0-9]{1,2})/([0-9]{1,6})/([0-9]{1,6})\.png")
PRIVATE_NETWORKS = tuple(
    ipaddress.IPv4Network(network)
    for network in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
)
REQUEST_HEADERS = ("Referer", "If-None-Match", "If-Modified-Since", "Cache-Control", "Pragma")
RESPONSE_HEADERS = (
    "Date", "Cache-Control", "Expires", "Age", "ETag", "Last-Modified", "Vary", "Retry-After",
)
HEALTH_PAGE = """<!doctype html>
<html lang="ru"><meta charset="utf-8"><title>OMMesh · карта</title>
<h1>Мост карты OMMesh доступен</h1>
<p>Вы открыли служебную страницу компьютера. Она не запрашивает данные из интернета.</p>
<p><a href="/tiles/12/2882/1298.png">Открыть одну проверочную плитку</a></p>
<p>Если плитка открывается на компьютере, попробуйте открыть эту служебную страницу
в браузере Nokia. Если страница не открывается на Nokia, проверьте выбранный IPv4,
общую сеть Wi-Fi и разрешение входящих подключений TCP к Python в брандмауэре.</p>
<p><a href="https://www.openstreetmap.org/copyright">© OpenStreetMap contributors</a></p>
</html>
""".encode("utf-8")


def lan_address(value):
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError as error:
        raise argparse.ArgumentTypeError("expected a numeric LAN IPv4 address") from error
    if not (address.is_loopback or any(address in network for network in PRIVATE_NETWORKS)):
        raise argparse.ArgumentTypeError("use a private LAN or loopback IPv4 address")
    return str(address)


def valid_contact(contact):
    if not re.fullmatch(r"[!-~]{1,200}", contact) or any(char in contact for char in "<>()"):
        return False
    if re.fullmatch(r"[a-zA-Z0-9_.+%-]+@[a-zA-Z0-9-]+(?:\.[a-zA-Z0-9-]+)+", contact):
        return True
    try:
        url = urllib.parse.urlsplit(contact)
        return url.scheme == "https" and bool(url.hostname) and url.username is None
    except ValueError:
        return False


def windows_lan_addresses():
    if sys.platform != "win32":
        return []
    query = """
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$addresses = @(Get-NetIPConfiguration -All -ErrorAction Stop | ForEach-Object {
    if ($_.NetAdapter.Status -eq 'Up') {
        $interface = $_.InterfaceAlias
        $_.IPv4Address | Where-Object { $_.AddressState -eq 'Preferred' } | ForEach-Object {
            [PSCustomObject]@{address = $_.IPAddress; interface = $interface}
        }
    }
})
ConvertTo-Json -InputObject $addresses -Compress
"""
    try:
        result = subprocess.run(
            ["powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command", query],
            check=True, capture_output=True, encoding="utf-8", timeout=10,
        )
        records = json.loads(result.stdout)
    except (OSError, ValueError, subprocess.SubprocessError):
        return []
    if not isinstance(records, list):
        return []
    addresses = {}
    for record in records:
        try:
            address = ipaddress.IPv4Address(record["address"])
        except (ipaddress.AddressValueError, KeyError, TypeError):
            continue
        if any(address in network for network in PRIVATE_NETWORKS):
            addresses[str(address)] = record.get("interface") or ""
    return list(addresses.items())


def interactive_setup():
    print("Убедитесь, что компьютер и Nokia находятся в одной локальной сети.")
    addresses = windows_lan_addresses()
    if addresses:
        print("Выберите интерфейс той сети, к которой подключена Nokia, например Wi-Fi или Ethernet.")
        for number, (address, interface) in enumerate(addresses, 1):
            print(f"  {number}. {address}  {interface}")
    else:
        print("Посмотрите IPv4 компьютера в параметрах сети или в выводе ipconfig.")

    choices = {str(number): address for number, (address, _) in enumerate(addresses, 1)}
    default_address = addresses[0][0] if len(addresses) == 1 else ""
    prompt = "Номер интерфейса или IPv4 компьютера"
    if default_address:
        prompt += f" [{default_address}]"
    while True:
        value = input(prompt + "> ").strip()
        value = choices.get(value, value or default_address)
        try:
            address = lan_address(value)
            if not ipaddress.IPv4Address(address).is_loopback:
                break
        except argparse.ArgumentTypeError:
            pass
        print("Укажите частный IPv4 компьютера, например 192.168.1.20.")

    return address


def upstream_template(template, contact):
    tokens = ("{z}", "{x}", "{y}")
    if not re.fullmatch(r"[!-~]+", template) or "\\" in template or "#" in template:
        raise ValueError("expected a printable HTTPS tile template")
    try:
        url = urllib.parse.urlsplit(template)
        host = url.hostname
        port = url.port
    except ValueError as error:
        raise ValueError("invalid tile upstream URL") from error
    if url.scheme != "https" or not host or url.username is not None or port == 0:
        raise ValueError("the tile upstream must be HTTPS without URL credentials")
    for token in tokens:
        if url.path.count(token) != 1 or template.count(token) != 1:
            raise ValueError("the upstream path must contain {z}, {x}, and {y} once each")
    if re.search(r"[{}]", re.sub(r"\{[zxy]\}", "", template)):
        raise ValueError("unexpected braces in tile template")

    host = host.rstrip(".")
    osm = host == "tile.openstreetmap.org" or host.endswith(".tile.openstreetmap.org")
    if osm and template != OSM_TEMPLATE:
        raise ValueError("use the official OpenStreetMap HTTPS tile URL")
    if contact and not valid_contact(contact):
        raise ValueError("use an email address or an HTTPS contact URL")
    return template, osm


def safe_header(value):
    return len(value) <= 4096 and all(char == "\t" or " " <= char <= "~" for char in value)


def tile_coordinates(path):
    route = TILE_PATH.fullmatch(path)
    if not route:
        return None
    z, x, y = map(int, route.groups())
    if 1 <= z <= 17 and x < (1 << z) and y < (1 << z):
        return z, x, y
    return None


def read_png(upstream):
    if upstream.headers.get_content_type() != "image/png":
        raise ValueError("expected PNG content")
    if upstream.headers.get("Content-Encoding", "identity").lower() != "identity":
        raise ValueError("expected identity content encoding")
    length = upstream.headers.get("Content-Length")
    if length is not None and (not length.isdecimal() or int(length) > MAX_TILE_BYTES):
        raise ValueError("invalid tile content length")
    data = upstream.read(MAX_TILE_BYTES + 1)
    signature_and_dimensions = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x01\x00\x00\x00\x01\x00"
    if not 33 <= len(data) <= MAX_TILE_BYTES or not data.startswith(signature_and_dimensions):
        raise ValueError("expected a 256×256 PNG tile within the phone's size limit")
    if length is not None and len(data) != int(length):
        raise ValueError("incomplete tile body")
    return data


class UnexpectedUpstreamStatus(ValueError):
    pass


def failure_detail(error):
    if isinstance(error, urllib.error.URLError):
        error = error.reason
    if isinstance(error, ssl.SSLCertVerificationError):
        return "Python не смог проверить сертификат сервера карт. Проверьте дату и системный прокси."
    if isinstance(error, ssl.SSLError):
        return "Python не смог установить защищённое соединение с сервером карт."
    if isinstance(error, socket.gaierror):
        return "Компьютер не смог определить адрес сервера карт. Проверьте интернет и DNS."
    if isinstance(error, TimeoutError):
        return "Сервер карт не ответил вовремя. Проверьте интернет на компьютере."
    if isinstance(error, UnexpectedUpstreamStatus):
        return str(error)
    if isinstance(error, ValueError):
        return "Сервер карт вернул ответ, который Nokia не сможет прочитать. Нужен PNG 256×256 до 160 КиБ."
    return "Компьютер не смог связаться с сервером карт. Проверьте интернет и системный прокси."


class NoRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class TileServer(HTTPServer):
    request_queue_size = 4

    def __init__(self, address, template, osm, contact):
        self.template = template
        self.osm = osm
        self.user_agent = USER_AGENT
        self.tile_clients = set()
        if contact:
            self.user_agent += " (contact: " + contact + ")"
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler(),
            urllib.request.HTTPSHandler(context=ssl.create_default_context()),
            NoRedirects(),
        )
        super().__init__(address, TileHandler)

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(TIMEOUT)
        return connection, address


class TileHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def reply(self, status, headers=(), data=b"", content_type=None):
        self.send_response_only(status)
        if not any(name == "Date" for name, value in headers):
            self.send_header("Date", self.date_time_string())
        for name, value in headers:
            self.send_header(name, value)
        if content_type is not None:
            self.send_header("Content-Type", content_type)
        if status != 304:
            self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if data:
            self.wfile.write(data)

    def forward(self, coordinates, headers):
        z, x, y = coordinates
        url = self.server.template.format(z=z, x=x, y=y)
        request = urllib.request.Request(url, headers=headers)
        try:
            response = self.server.opener.open(request, timeout=TIMEOUT)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            status = response.getcode()
            selected = [
                (name, value)
                for name in RESPONSE_HEADERS
                for value in response.headers.get_all(name, [])
                if safe_header(value)
            ]
            if status == 200:
                data = read_png(response)
                if self.server.osm and not any(
                    name in ("Cache-Control", "Expires") for name, value in selected
                ):
                    selected.append(("Cache-Control", "max-age=604800"))
                return status, selected, data
            if status == 304 or 400 <= status <= 599:
                return status, selected, b""
        raise UnexpectedUpstreamStatus(
            f"Сервер карт ответил HTTP {status}. Мосту нужен прямой HTTPS-адрес плиток."
        )

    def tile_failure(self, status, detail, headers=()):
        text = f"Плитка {self.path} · HTTP {status}\n{detail}\n"
        print(text.rstrip(), flush=True)
        self.reply(status, headers, text.encode("utf-8"), "text/plain; charset=utf-8")

    def do_GET(self):
        if self.path == "/":
            self.reply(200, (("Cache-Control", "no-store"),), HEALTH_PAGE,
                       "text/html; charset=utf-8")
            return
        coordinates = tile_coordinates(self.path)
        if coordinates is None:
            self.reply(404)
            return
        headers = {
            "User-Agent": self.server.user_agent,
            "Accept": "image/png",
            "Accept-Encoding": "identity",
        }
        for name in REQUEST_HEADERS:
            values = self.headers.get_all(name, [])
            if not values:
                continue
            combined = ", ".join(values)
            if not safe_header(combined):
                self.reply(400)
                return
            headers[name] = combined
        try:
            try:
                status, response_headers, data = self.forward(coordinates, headers)
            except (OSError, http.client.HTTPException, ValueError) as error:
                self.tile_failure(502, failure_detail(error))
                return
            if status >= 400:
                detail = {
                    403: "Сервер карт отказал в доступе.",
                    429: "Сервер карт ограничил частоту запросов. Повторите позже.",
                }.get(status, f"Сервер карт ответил HTTP {status}.")
                self.tile_failure(status, detail, response_headers)
                return
            self.reply(status, response_headers, data, "image/png" if status == 200 else None)
            peer = self.client_address[0]
            if peer not in self.server.tile_clients and len(self.server.tile_clients) < 16:
                self.server.tile_clients.add(peer)
                print(f"{peer} получил ответ на запрос плитки · HTTP {status}", flush=True)
        except OSError:
            self.close_connection = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interactive", action="store_true", help="set up an OpenStreetMap relay")
    parser.add_argument("--listen", type=lan_address, help="private LAN IPv4 address")
    parser.add_argument("--port", default=DEFAULT_PORT, type=int, help="LAN port (default 8765)")
    parser.add_argument(
        "--upstream",
        help="authorized HTTPS PNG tile URL template with {z}, {x}, and {y} in the path",
    )
    parser.add_argument("--contact", default="", help="optional operator email or HTTPS URL")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be 1–65535")
    if args.interactive:
        if args.listen or args.upstream:
            parser.error("--interactive prompts for --listen and uses OpenStreetMap")
        try:
            args.listen = interactive_setup()
        except (EOFError, KeyboardInterrupt):
            return
        args.upstream = OSM_TEMPLATE
    elif not (args.listen and args.upstream):
        parser.error("--listen and --upstream are required")
    try:
        template, osm = upstream_template(args.upstream, args.contact)
        server = TileServer((args.listen, args.port), template, osm, args.contact)
    except OSError as error:
        if error.errno == errno.EADDRINUSE or getattr(error, "winerror", None) == 10048:
            parser.error(f"Порт {args.port} уже занят. Проверьте, не запущена ли другая копия моста.")
        if error.errno == errno.EADDRNOTAVAIL or getattr(error, "winerror", None) == 10049:
            parser.error(f"Адрес {args.listen} отсутствует на компьютере. Проверьте выбранный интерфейс.")
        parser.error(str(error))
    except ValueError as error:
        parser.error(str(error))
    with server:
        address, port = server.server_address
        health_url = f"http://{address}:{port}/"
        url = "http://{}:{}/tiles/{{z}}/{{x}}/{{y}}.png".format(address, port)
        if args.interactive:
            phone_address = address if port == DEFAULT_PORT else f"{address}:{port}"
            print("На Nokia выберите Настройки → Интернет-карта → Через компьютер.", flush=True)
            print(f"В поле «Адрес компьютера» введите {phone_address}", flush=True)
            print(f"Полный адрес для старых версий приложения\n{url}", flush=True)
            print(f"Откройте {health_url} в браузере ПК для проверки.", flush=True)
            print("Оставьте это окно открытым. Для остановки нажмите Ctrl+C.", flush=True)
        else:
            print(f"Map template: {url}", flush=True)
            print(f"Browser check: {health_url}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
