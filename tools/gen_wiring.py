#!/usr/bin/env python3
"""Draw the programmer wiring diagrams for docs/hardware.md (own artwork, no third-party images).

    python tools/gen_wiring.py      -> docs/img/pi_header_wiring.{png,svg}, docs/img/swd_mouse.{png,svg}
"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, FancyBboxPatch, Rectangle

OUT = Path(__file__).resolve().parent.parent / "docs" / "img"

# Raspberry Pi 40-pin header, (pin) -> name
PINS = {
    1: "3V3", 2: "5V", 3: "GPIO2", 4: "5V", 5: "GPIO3", 6: "GND", 7: "GPIO4", 8: "GPIO14",
    9: "GND", 10: "GPIO15", 11: "GPIO17", 12: "GPIO18", 13: "GPIO27", 14: "GND", 15: "GPIO22",
    16: "GPIO23", 17: "3V3", 18: "GPIO24", 19: "GPIO10", 20: "GND", 21: "GPIO9", 22: "GPIO25",
    23: "GPIO11", 24: "GPIO8", 25: "GND", 26: "GPIO7", 27: "GPIO0", 28: "GPIO1", 29: "GPIO5",
    30: "GND", 31: "GPIO6", 32: "GPIO12", 33: "GPIO13", 34: "GND", 35: "GPIO19", 36: "GPIO16",
    37: "GPIO26", 38: "GPIO20", 39: "GND", 40: "GPIO21",
}
# pin -> (colour, text) for the used pins
MOUSE = {22: ("#1f6fd6", "1k/2k divider -> mouse pad CLK"), 18: ("#1a9a3a", "150 Ohm -> mouse pad DIO"),
         20: ("#222222", "GND")}
DONGLE = {1: ("#d62828", "V33 (3.3 V, dongle only!)"), 9: ("#222222", "GND"),
          11: ("#8a2be2", "1k -> dongle pad SWDIO"),
          13: ("#11998e", "1k -> dongle pad SWCLK"),
          15: ("#aaaaaa", "not needed (was a probe candidate)")}


def header():
    fig, ax = plt.subplots(figsize=(11, 22.5))
    ax.set_xlim(0, 11)
    ax.set_ylim(-1.6, 21.5)
    ax.set_aspect("equal")
    ax.axis("off")
    ax.add_patch(FancyBboxPatch((4.55, 0.3), 1.9, 20.4, boxstyle="round,pad=0.1", fc="#2b2b2b"))
    for pin, name in PINS.items():
        row = (pin - 1) // 2
        y = 20.2 - row
        left = pin % 2 == 1
        x = 5.0 if left else 6.0
        used = MOUSE.get(pin) or DONGLE.get(pin)
        ax.add_patch(Circle((x, y), 0.33, fc=used[0] if used else "#dddddd", ec="white", lw=1.2))
        ax.text(x, y, str(pin), ha="center", va="center", fontsize=7,
                color="white" if used else "#333333", weight="bold")
        tx = 4.35 if left else 6.65
        ax.text(tx, y, name, ha="right" if left else "left", va="center", fontsize=8,
                color=used[0] if used else "#777777", weight="bold" if used else "normal")
        if used:
            lx = 0.1 if left else 10.9
            ax.plot([tx - (0.95 if left else -0.95), lx + (1.9 if left else -1.9)], [y, y],
                    color=used[0], lw=1.2)
            ax.text(lx, y + 0.28, used[1], ha="left" if left else "right", va="bottom",
                    fontsize=8, color=used[0])
    ax.text(0.1, 21.1, "DONGLE (3.3 V) - left row", fontsize=12, weight="bold")
    ax.text(10.9, 21.1, "MOUSE (2.1 V!) - right row", fontsize=12, weight="bold", ha="right")
    ax.text(5.5, -0.6, "Raspberry Pi 4 GPIO header seen from above. Pin 1 (square pad) is at the "
            "end farthest from the USB/Ethernet ports.",
            ha="center", fontsize=9, color="#555555")
    ax.text(5.5, -1.25, "Never connect 5 V (pins 2/4) or the Pi's 3.3 V to the mouse; it runs "
            "from its own battery.", ha="center", fontsize=9, color="#b00020", weight="bold")
    return fig


def resistor(ax, x, y, w=1.0, label=""):
    ax.add_patch(Rectangle((x, y - 0.18), w, 0.36, fc="white", ec="black", lw=1.5))
    ax.text(x + w / 2, y + 0.3, label, ha="center", va="bottom", fontsize=9)


def mouse_schematic():
    fig, ax = plt.subplots(figsize=(10, 4.6))
    ax.set_xlim(0, 10)
    ax.set_ylim(0, 4.6)
    ax.axis("off")
    # CLK: pin22 -> 1k -> node -> mouse SWDCLK ; node -> 2k -> GND
    y = 3.6
    ax.text(0.1, y, "Pi pin 22\nGPIO25", va="center", fontsize=10, color="#1f6fd6", weight="bold")
    ax.plot([1.4, 2.4], [y, y], "k", lw=1.5)
    resistor(ax, 2.4, y, 1.0, "1 k")
    ax.plot([3.4, 8.3], [y, y], "k", lw=1.5)
    ax.plot(5.2, y, "ko", ms=5)
    ax.plot([5.2, 5.2], [y, y - 0.6], "k", lw=1.5)
    ax.add_patch(Rectangle((5.02, y - 1.6), 0.36, 1.0, fc="white", ec="black", lw=1.5))
    ax.text(5.5, y - 1.1, "2 k\n(2 x 1 k)", va="center", fontsize=9)
    ax.plot([5.2, 5.2], [y - 1.6, y - 2.0], "k", lw=1.5)
    ax.plot([4.9, 5.5], [y - 2.0, y - 2.0], "k", lw=2)
    ax.text(5.6, y - 2.05, "GND", va="center", fontsize=9)
    ax.text(8.4, y, "mouse SWDCLK\n(about 2.2 V high)", va="center", fontsize=10, weight="bold")
    # DIO
    y = 1.3
    ax.text(0.1, y, "Pi pin 18\nGPIO24", va="center", fontsize=10, color="#1a9a3a", weight="bold")
    ax.plot([1.4, 2.4], [y, y], "k", lw=1.5)
    resistor(ax, 2.4, y, 1.0, "150 Ohm")
    ax.plot([3.4, 8.3], [y, y], "k", lw=1.5)
    ax.text(8.4, y, "mouse SWDIO", va="center", fontsize=10, weight="bold")
    ax.text(3.6, y - 0.55, "OpenOCD drives GPIO24 open-drain (low or released): the high level\n"
            "comes from the nRF52840's pull-up at 2.1 V, so 3.3 V never reaches the mouse.",
            fontsize=8.5, color="#555555", va="top")
    ax.text(0.1, 0.05, "Pi pin 20 (GND) -> mouse GND.   Config: pi/pulsedart_nolvl.cfg, speed 100 kHz.",
            fontsize=9)
    return fig


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, fig in (("pi_header_wiring", header()), ("swd_mouse", mouse_schematic())):
        for ext in ("png", "svg"):
            fig.savefig(OUT / f"{name}.{ext}", dpi=130, bbox_inches="tight", facecolor="white")
        plt.close(fig)
    print("written to", OUT)


if __name__ == "__main__":
    main()
