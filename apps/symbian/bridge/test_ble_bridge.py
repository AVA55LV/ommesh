import asyncio
from contextlib import suppress
import unittest

from ble_bridge import BridgeSession, FRAME_LIMIT, UART_NOTIFY, UART_WRITE


class FakeBleError(Exception):
    pass


class FakeRadio:
    def __init__(self):
        self.writes = asyncio.Queue()
        self.callback = None

    async def start_notify(self, characteristic, callback):
        assert characteristic == UART_NOTIFY
        self.callback = callback

    async def write_gatt_char(self, characteristic, frame, *, response):
        assert characteristic == UART_WRITE and response
        self.writes.put_nowait(frame)

    def notify(self, data):
        self.callback(UART_NOTIFY, bytearray(data))


class BridgeTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.radio = FakeRadio()
        self.disconnected = asyncio.Event()
        self.session = BridgeSession(self.radio, UART_NOTIFY, UART_WRITE, FakeBleError)
        address = asyncio.get_running_loop().create_future()
        self.server = asyncio.create_task(
            self.session.serve("127.0.0.1", 0, self.disconnected, address.set_result)
        )
        self.address = await asyncio.wait_for(address, 3)
        self.reader, self.writer = await asyncio.open_connection(*self.address)

    async def asyncTearDown(self):
        self.writer.close()
        with suppress(OSError):
            await self.writer.wait_closed()
        self.disconnected.set()
        await asyncio.wait_for(self.server, 3)

    async def test_split_and_combined_tcp_frames_and_multiple_notifications(self):
        stamp = b"\x16\x03"
        self.writer.write(b"<\x02")
        await self.writer.drain()
        await asyncio.sleep(0)
        large = bytes(range(FRAME_LIMIT))
        self.writer.write(b"\x00" + stamp + b"<" + len(large).to_bytes(2, "little") + large)
        await self.writer.drain()
        self.assertEqual(await asyncio.wait_for(self.radio.writes.get(), 3), stamp)
        self.assertEqual(await asyncio.wait_for(self.radio.writes.get(), 3), large)

        self.radio.notify(b"\x0d\x00><")
        self.radio.notify(b"\x83")
        actual = await asyncio.wait_for(self.reader.readexactly(11), 3)
        self.assertEqual(actual, b">\x04\x00\x0d\x00><>\x01\x00\x83")

    async def test_refuses_second_client_and_rejects_invalid_length(self):
        self.writer.write(b"<\x01\x00\x04")
        await self.writer.drain()
        self.assertEqual(await asyncio.wait_for(self.radio.writes.get(), 3), b"\x04")

        second, second_writer = await asyncio.open_connection(*self.address)
        try:
            self.assertEqual(await asyncio.wait_for(second.read(), 3), b"")
        finally:
            second_writer.close()
            await second_writer.wait_closed()

        self.writer.write(b"<\xb1\x00")
        await self.writer.drain()
        self.assertEqual(await asyncio.wait_for(self.reader.read(), 3), b"")
        self.assertTrue(self.radio.writes.empty())

    async def test_radio_disconnect_closes_tcp(self):
        self.writer.write(b"<\x01\x00\x0a")
        await self.writer.drain()
        self.assertEqual(await asyncio.wait_for(self.radio.writes.get(), 3), b"\x0a")
        self.disconnected.set()
        self.assertEqual(await asyncio.wait_for(self.reader.read(), 3), b"")

    async def test_incomplete_frame_is_never_sent_to_ble(self):
        self.writer.write(b"<\x02\x00\x16")
        await self.writer.drain()
        self.writer.write_eof()
        self.assertEqual(await asyncio.wait_for(self.reader.read(), 3), b"")
        self.assertTrue(self.radio.writes.empty())


if __name__ == "__main__":
    unittest.main()
