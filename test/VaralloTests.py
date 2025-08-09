from enum import Enum, auto

class T(Enum):
    A_1 = auto()
    B_1 = auto()
    C_1 = auto()
    Piste = auto()
    C_2 = auto()
    B_2 = auto()
    A_2 = auto()

# Map enum to string for your existing logic
T_str = {t: t.name for t in T}

def conn(*args):
    # Convert enums to their string names, keep "R" as is
    return tuple(T_str[a] if isinstance(a, T) else a for a in args)

terminals = ["A_1", "B_1", "C_1", "Piste", "C_2", "B_2", "A_2"]

distances_mm = {
    ("A_1", "B_1"): 15,
    ("B_1", "C_1"): 20,
    ("C_1", "Piste"): 30,
    ("Piste", "C_2"): 30,
    ("C_2", "B_2"): 20,
    ("B_2", "A_2"): 15,
}

scale = 0.5  # mm to characters

positions = {}
center_pos = 50
positions["Piste"] = center_pos

idx_piste = terminals.index("Piste")
pos = center_pos
for i in range(idx_piste - 1, -1, -1):
    left = terminals[i]
    right = terminals[i + 1]
    dist = int(round(distances_mm[(left, right)] * scale))
    pos -= dist
    positions[left] = pos

pos = center_pos
for i in range(idx_piste + 1, len(terminals)):
    left = terminals[i - 1]
    right = terminals[i]
    dist = int(round(distances_mm[(left, right)] * scale))
    pos += dist
    positions[right] = pos

label_widths = {t: 5 if t == "Piste" else 3 for t in terminals}


connections10_1a = [
    conn(T.A_1, T.B_1)
]
connections10_1b = [
    conn(T.A_2, T.B_2)
]

connections10_2a = [
    conn(T.A_1, T.B_1, "R")
]

connections10_2b = [
    conn(T.A_1, T.B_1, "R")
]

connections12_1a = [
    conn(T.A_1, T.B_1),
    conn(T.B_1, T.C_2, "R")
]

connections12_1b = [
    conn(T.A_2, T.B_2),
    conn(T.B_2, T.C_1, "R")
]

onnections12_2a = [
    conn(T.A_1, T.B_1),
    conn(T.B_1, T.C_2, "R")
]

connections12_2b = [
    conn(T.A_2, T.B_2),
    conn(T.B_2, "Piste", "R")
]

connections5_1 = [
    conn(T.B_1, T.A_2),
    conn(T.A_2, T.C_1, "R"),
    # etc...
]

# Use the desired connections array:
connections = connections12_2b


def draw_diagram(terminals, positions, label_widths, connections):
    width = max(positions.values()) + 10
    canvas = [[" "] * width for _ in range(4)]
    label_centers = {}

    for t in terminals:
        w = label_widths[t]
        center = positions[t]
        start = center - w // 2
        for i, ch in enumerate(t):
            if start + i < width:
                canvas[0][start + i] = ch
        label_centers[t] = center

    for conn in connections:
        frm, to = conn[0], conn[1]
        resistor = len(conn) == 3 and conn[2] == "R"
        pos_from = label_centers[frm]
        pos_to = label_centers[to]
        left, right = sorted([pos_from, pos_to])
        idx_from = terminals.index(frm)
        idx_to = terminals.index(to)

        if abs(idx_from - idx_to) == 1:
            length = right - left - 1
            if resistor:
                seg = list("-R-".center(length, "-"))
            else:
                seg = ["-"] * length
            for i, ch in enumerate(seg):
                pos = left + 1 + i
                overwrite = True
                for t in terminals:
                    tw = label_widths[t]
                    ts = positions[t] - tw // 2
                    te = ts + tw
                    if ts <= pos < te:
                        overwrite = False
                        break
                if overwrite and pos < width:
                    canvas[0][pos] = ch
        else:
            canvas[1][pos_from] = "|"
            canvas[1][pos_to] = "|"
            canvas[2][pos_from] = "|"
            canvas[2][pos_to] = "|"
            length = right - left - 1
            if resistor:
                seg = list("-R-".center(length, "-"))
            else:
                seg = ["-"] * length
            for i, ch in enumerate(seg):
                pos = left + 1 + i
                if pos < width:
                    canvas[2][pos] = ch

    return ["".join(row).rstrip() for row in canvas]

diagram = draw_diagram(terminals, positions, label_widths, connections)
for line in diagram:
    print(line)

# Enumerate and draw all defined connections arrays
for name, value in list(globals().items()):
    if name.startswith("connections") and isinstance(value, list):
        ident = name[len("connections"):]  # Get the part after 'connections'
        print(f"connections{ident}:")
        diagram = draw_diagram(terminals, positions, label_widths, value)
        for line in diagram:
            print(line)
        print()