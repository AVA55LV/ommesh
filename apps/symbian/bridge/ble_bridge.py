#!/usr/bin/env python3
"""Relay TCP companion frames to one MeshCore BLE radio."""

import argparse
import asyncio
from ipaddress import IPv4Address
import sys


UART_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
UART_WRITE = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
UART_NOTIFY = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
FRAME_LIMIT = 176


class BridgeError(Exception):
    pass


async def receive_frame(reader):
    marker = await reader.read(1)
    if not marker:
        return None
    if marker != b"<":
        raise BridgeError("ожидался маркер '<' в начале кадра")
    try:
        async with asyncio.timeout(5):
            size = int.from_bytes(await reader.readexactly(2), "little")
            if not 1 <= size <= FRAME_LIMIT:
                raise BridgeError(f"недопустимый размер TCP-кадра: {size}")
            return await reader.readexactly(size)
    except (TimeoutError, asyncio.IncompleteReadError) as error:
        raise BridgeError("TCP-кадр пришёл не целиком") from error


class BridgeSession:
    def __init__(self, radio, receive_char, send_char, ble_error):
        self.radio = radio
        self.receive_char = receive_char
        self.send_char = send_char
        self.ble_error = ble_error
        self.notifications = asyncio.Queue(maxsize=512)
        self.ended = asyncio.Event()
        self.client = None
        self.writer = None

    def notify(self, _characteristic, data):
        if self.client is None or self.ended.is_set() or not data:
            return
        try:
            if len(data) > FRAME_LIMIT:
                raise BridgeError(f"слишком длинное BLE-уведомление: {len(data)}")
            self.notifications.put_nowait(bytes(data))
        except (BridgeError, asyncio.QueueFull) as error:
            detail = str(error) or "очередь BLE-уведомлений заполнена"
            print(f"Соединение закрыто ({detail}).", file=sys.stderr, flush=True)
            self.ended.set()

    def accept(self, reader, writer):
        if self.client is not None or self.ended.is_set():
            writer.close()
            return
        self.writer = writer
        self.client = asyncio.create_task(self.relay(reader, writer))

    async def tcp_to_ble(self, reader):
        while (frame := await receive_frame(reader)) is not None:
            await asyncio.wait_for(
                self.radio.write_gatt_char(self.send_char, frame, response=True), 40
            )

    async def ble_to_tcp(self, writer):
        while True:
            frame = await self.notifications.get()
            writer.write(b">" + len(frame).to_bytes(2, "little") + frame)
            await writer.drain()

    async def relay(self, reader, writer):
        print("Клиент MeshCore подключён.", flush=True)
        tasks = [
            asyncio.create_task(self.tcp_to_ble(reader)),
            asyncio.create_task(self.ble_to_tcp(writer)),
        ]
        try:
            finished, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in finished:
                task.result()
        except (BridgeError, OSError, self.ble_error) as error:
            detail = str(error) or type(error).__name__
            print(f"Соединение закрыто ({detail}).", file=sys.stderr, flush=True)
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), 2)
            except OSError:
                writer.transport.abort()
            finally:
                self.ended.set()

    async def serve(self, host, port, radio_disconnected, listening):
        await asyncio.wait_for(self.radio.start_notify(self.receive_char, self.notify), 20)
        async with await asyncio.start_server(self.accept, host, port, limit=65536) as server:
            listening(server.sockets[0].getsockname())
            end_waiters = [
                asyncio.create_task(self.ended.wait()),
                asyncio.create_task(radio_disconnected.wait()),
            ]
            try:
                await asyncio.wait(end_waiters, return_when=asyncio.FIRST_COMPLETED)
            finally:
                server.close()
                self.ended.set()
                for waiter in end_waiters:
                    waiter.cancel()
                if self.client is not None:
                    self.writer.transport.abort()
                    self.client.cancel()
                    end_waiters.append(self.client)
                await asyncio.gather(*end_waiters, return_exceptions=True)


async def scan(scanner, timeout):
    adverts = await scanner.discover(
        timeout=timeout, service_uuids=[UART_SERVICE], return_adv=True
    )
    for device, advert in adverts.values():
        print(f"{device.address}  {advert.local_name or device.name or 'MeshCore'}")
    if not adverts:
        print("Устройства с BLE-сервисом MeshCore не найдены.", file=sys.stderr)
    return bool(adverts)


async def run(args, client_type, scanner, ble_error):
    ready_once = False
    loop = asyncio.get_running_loop()

    def listening(address):
        nonlocal ready_once
        ready_once = True
        host, port = address
        print(f"Радио готово. Адрес TCP для клиента — {host}:{port}", flush=True)

    while True:
        radio_disconnected = asyncio.Event()

        def disconnected(_radio, event=radio_disconnected):
            loop.call_soon_threadsafe(event.set)

        try:
            device = await scanner.find_device_by_address(
                args.device, timeout=args.scan_timeout, service_uuids=[UART_SERVICE]
            )
            if device is None:
                raise BridgeError(f"радио {args.device} не найдено")

            async with client_type(
                device, disconnected_callback=disconnected, pair=True, timeout=60
            ) as radio:
                service = radio.services.get_service(UART_SERVICE)
                if service is None:
                    raise BridgeError("сервис MeshCore UART недоступен")
                receive = service.get_characteristic(UART_NOTIFY)
                send = service.get_characteristic(UART_WRITE)
                if (
                    receive is None or "notify" not in receive.properties
                    or send is None or "write" not in send.properties
                ):
                    raise BridgeError("характеристики MeshCore UART недоступны")
                session = BridgeSession(radio, receive, send, ble_error)
                await session.serve(str(args.listen), args.port, radio_disconnected, listening)
        except (BridgeError, ble_error, TimeoutError) as error:
            if not ready_once:
                raise BridgeError(
                    f"{str(error) or type(error).__name__}. Проверьте BLE, PIN и другие "
                    "подключения к радио."
                ) from error
            print(f"Радио сейчас недоступно ({error}).", file=sys.stderr, flush=True)

        print("Восстанавливаю связь с радио.", flush=True)
        await asyncio.sleep(2)


def parse_port(value):
    try:
        port = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("укажите номер порта от 1 до 65535") from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("укажите номер порта от 1 до 65535")
    return port


def parse_listen_address(value):
    try:
        address = IPv4Address(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("укажите IPv4-адрес компьютера") from error
    if address.is_unspecified or address.is_multicast:
        raise argparse.ArgumentTypeError("укажите адрес конкретного сетевого интерфейса")
    return address


def main():
    parser = argparse.ArgumentParser(description="Мост MeshCore BLE ↔ TCP.")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--scan", action="store_true", help="найти BLE-радио")
    action.add_argument("--device", help="MAC (Linux/Windows) или UUID (macOS) радио из --scan")
    parser.add_argument("--listen", type=parse_listen_address, default="127.0.0.1",
                        help="локальный IPv4-адрес компьютера (по умолчанию 127.0.0.1)")
    parser.add_argument("--port", type=parse_port, default=5000, help="TCP-порт (по умолчанию 5000)")
    parser.add_argument("--scan-timeout", type=int, choices=range(1, 61), default=10,
                        metavar="СЕКУНДЫ", help="длительность поиска, от 1 до 60 секунд")
    args = parser.parse_args()

    try:
        from bleak import BleakClient, BleakScanner
        from bleak.exc import BleakError
    except ImportError:
        print("Установите зависимости: python -m pip install -r requirements.txt", file=sys.stderr)
        return 1

    try:
        if args.scan:
            return 0 if asyncio.run(scan(BleakScanner, args.scan_timeout)) else 1
        asyncio.run(run(args, BleakClient, BleakScanner, BleakError))
    except KeyboardInterrupt:
        return 0
    except (OSError, BridgeError, BleakError, TimeoutError) as error:
        print(str(error) or type(error).__name__, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
