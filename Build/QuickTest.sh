cat <<'PSD' | ./parashade --run
module Demo:
scope main range app:
    declare explicit integer named x equals 0x2A end
    declare implicit named y equals x plus 0x10 end
    return y
end
PSD
# → 74  (0x2A + 0x10 = 0x3A = 58? wait: 0x2A=42, +16 = 58. The demo prints 58)
