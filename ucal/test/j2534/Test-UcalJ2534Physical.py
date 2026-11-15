"""Check a J2534 endpoint and an independent PCAN-Basic or Vector XL hardware peer."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import uuid


FRAME_COUNT = 16
TX_ID = 0x5A5
RX_ID = 0x5A6


def physical_peer(can, interface, channel, fd):
    timing = can.BitTimingFd(f_clock=80_000_000, nom_brp=2, nom_tseg1=63,
                             nom_tseg2=16, nom_sjw=16, data_brp=2,
                             data_tseg1=15, data_tseg2=4, data_sjw=4)
    parameters = {"bitrate": 500_000, "fd": fd}
    if fd:
        parameters["timing"] = timing
    if interface == "vector":
        from can.interfaces.vector import get_channel_configs
        channel = int(channel)
        matches = [device for device in get_channel_configs()
                   if device.channel_index == channel and not device.is_on_bus
                   and device.hw_type.name != "XL_HWTYPE_VIRTUAL"]
        parameters["app_name"] = None
    else:
        matches = [device for device in can.detect_available_configs("pcan")
                   if device["channel"] == channel and device.get("supports_fd")
                   and device.get("channel_condition") == 1]
    if len(matches) != 1:
        raise RuntimeError("The selected independent physical peer is unavailable or in use")
    return can.Bus(interface=interface, channel=channel,
                   can_filters=[{"can_id": TX_ID, "can_mask": 0x7ff, "extended": False}],
                   **parameters)


def exchange(can, peer, fd):
    size = 64 if fd else 8
    for index in range(FRAME_COUNT):
        payload = index.to_bytes(4, "little") + bytes(
            0x80 + ((index + offset) & 0x3f) for offset in range(4, size))
        frame = peer.recv(timeout=2)
        if (frame is None or frame.arbitration_id != TX_ID or frame.is_fd != fd
                or frame.bitrate_switch != fd or bytes(frame.data) != payload):
            raise RuntimeError(f"Independent peer did not receive the expected frame {index}")
        peer.send(can.Message(arbitration_id=RX_ID, is_extended_id=False,
                              is_fd=fd, bitrate_switch=fd, data=payload), timeout=1)
    if peer.recv(timeout=.15) is not None:
        raise RuntimeError("Independent peer received a duplicate frame")


def stop_on_failure(process):
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-bin", required=True, type=Path,
                        help="Directory containing UcalBroker.exe and UcalHarness.exe")
    parser.add_argument("--worker", type=Path, help="Broker executable matching the vendor DLL bitness")
    parser.add_argument("--provider-config", required=True, type=Path)
    parser.add_argument("--endpoint", required=True, type=uuid.UUID)
    parser.add_argument("--peer-interface", required=True, choices=("pcan", "vector"))
    parser.add_argument("--peer-channel", required=True)
    parser.add_argument("--mode", choices=("fd", "classic"), default="fd")
    parser.add_argument("--evidence-directory", required=True, type=Path)
    parser.add_argument("--isolated-bus", required=True, action="store_true",
                        help="Confirm that only test equipment is connected to the CAN bus")
    args = parser.parse_args()
    import can

    broker = (args.build_bin / "UcalBroker.exe").resolve(strict=True)
    harness = (args.build_bin / "UcalHarness.exe").resolve(strict=True)
    worker = (args.worker or broker).resolve(strict=True)
    config = args.provider_config.resolve(strict=True)
    evidence = args.evidence_directory.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    traffic = evidence / "traffic.jsonl"
    fd = args.mode == "fd"
    env = {key: value for key, value in os.environ.items() if not key.startswith("UCAL_")}
    env.update(UCAL_BACKEND="J2534", UCAL_BROKER_INSTANCE="j2534_peer_" + uuid.uuid4().hex,
               UCAL_J2534_WORKER_PATH=str(worker), UCAL_J2534_CONFIG=str(config),
               UCAL_J2534_DIAGNOSTIC_PATH=str(evidence / "driver-errors.jsonl"),
               UCAL_HARNESS_ENDPOINT_ID=str(args.endpoint), UCAL_HARNESS_CAN_FD=str(int(fd)),
               UCAL_HARNESS_NBITRATE="500000", UCAL_HARNESS_DBITRATE="2000000",
               UCAL_HARNESS_PAYLOAD_LENGTH="64" if fd else "8", UCAL_HARNESS_FRAME_COUNT=str(FRAME_COUNT),
               UCAL_HARNESS_INTER_FRAME_US="10000", UCAL_HARNESS_TIMEOUT_MS="6000",
               UCAL_HARNESS_TX_FRAME_ID=hex(TX_ID), UCAL_HARNESS_RX_FRAME_ID=hex(RX_ID))

    def start(executable, argument, output, errors):
        return subprocess.Popen([str(executable), argument], env=env,
                                creationflags=subprocess.CREATE_NO_WINDOW,
                                stdout=output, stderr=errors)

    with (evidence / "broker.log").open("w") as log, traffic.open("w") as output:
        with physical_peer(can, args.peer_interface, args.peer_channel, fd) as peer:
            server = start(broker, "--allow-test-shutdown", log, log)
            try:
                runner = start(harness, "can-hardware-smoke", output, log)
                try:
                    exchange(can, peer, fd)
                    if runner.wait(timeout=9) != 0:
                        raise RuntimeError("UCAL harness did not complete the traffic and close checks")
                finally:
                    stop_on_failure(runner)
            finally:
                try:
                    shutdown = start(harness, "shutdown", log, log)
                    try:
                        if shutdown.wait(timeout=9) != 0 or server.wait(timeout=9) != 0:
                            raise RuntimeError("UCAL Broker did not shut down normally")
                    finally:
                        stop_on_failure(shutdown)
                finally:
                    stop_on_failure(server)

    results = [json.loads(line) for line in traffic.read_text(encoding="utf-8").splitlines()]
    result = results[-1]
    if (not result["canHardwareSmoke"] or result["submittedCount"] != FRAME_COUNT
            or result["receivedCount"] != FRAME_COUNT or not result["closed"]):
        raise RuntimeError("UCAL did not confirm complete bidirectional delivery and normal close")
    print(json.dumps({"mode": args.mode, "peerReceived": FRAME_COUNT,
                      "ucalReceived": result["receivedCount"], "closed": result["closed"]}))


if __name__ == "__main__":
    main()
