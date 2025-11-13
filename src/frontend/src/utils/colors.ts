const clamp = (v: number, a: number, b: number) => Math.min(Math.max(v, a), b);

const hexToRgb = (hex: string) => {
  const m = hex.replace("#", "").match(/^([0-9a-f]{3}|[0-9a-f]{6})$/i);
  if (!m) throw new Error("Bad hex");
  let s = m[1];
  if (s.length === 3) s = s.split("").map((c) => c + c).join("");
  const n = parseInt(s, 16);
  return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
};

const rgbToHex = ({ r, g, b }: { r: number; g: number; b: number }) =>
  "#" +
  [r, g, b]
    .map((v) => v.toString(16).padStart(2, "0"))
    .join("");

const rgbToHsl = ({ r, g, b }: { r: number; g: number; b: number }) => {
  r /= 255;
  g /= 255;
  b /= 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  let h = 0;
  let s = 0;
  const l = (max + min) / 2;
  if (max !== min) {
    const d = max - min;
    s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    switch (max) {
      case r:
        h = (g - b) / d + (g < b ? 6 : 0);
        break;
      case g:
        h = (b - r) / d + 2;
        break;
      default:
        h = (r - g) / d + 4;
        break;
    }
    h *= 60;
  }
  return { h, s: s * 100, l: l * 100 };
};

const hslToRgb = ({ h, s, l }: { h: number; s: number; l: number }) => {
  s /= 100;
  l /= 100;
  const c = (1 - Math.abs(2 * l - 1)) * s;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  const m = l - c / 2;
  let r1 = 0;
  let g1 = 0;
  let b1 = 0;
  if (0 <= h && h < 60) [r1, g1, b1] = [c, x, 0];
  else if (60 <= h && h < 120) [r1, g1, b1] = [x, c, 0];
  else if (120 <= h && h < 180) [r1, g1, b1] = [0, c, x];
  else if (180 <= h && h < 240) [r1, g1, b1] = [0, x, c];
  else if (240 <= h && h < 300) [r1, g1, b1] = [x, 0, c];
  else [r1, g1, b1] = [c, 0, x];
  return {
    r: Math.round((r1 + m) * 255),
    g: Math.round((g1 + m) * 255),
    b: Math.round((b1 + m) * 255),
  };
};

export type DarkishParams = {
  bases?: string[];
  hueOffset?: number;
  hueJitter?: number;
  sat?: [number, number];
  light?: [number, number];
};

export function randomDarkishColor({
  bases = ['#aabbcc', '#bbaacc', '#ccaabb', '#ddaabb', '#eeaabb'],
  hueOffset = 4,
  hueJitter = 8,
  sat = [0, 75],
  light = [23, 40],
}: DarkishParams = {}): string {
  const baseHex = bases[Math.floor(Math.random() * bases.length)];
  const base = rgbToHsl(hexToRgb(baseHex));

  const sign = Math.random() < 0.5 ? -1 : 1;
  const away = (base.h + sign * hueOffset + 360) % 360;
  const h = (away + (Math.random() * 2 - 1) * hueJitter + 360) % 360;

  const [satMin, satMax] = sat;
  const [lMin, lMax] = light;
  const s = clamp(satMin + (satMax - satMin) * Math.random(), 0, 100);

  const wave = (Math.sin(Math.random() * 6.28318) + 1) / 2;
  const l = clamp(lMin + (lMax - lMin) * (0.25 + 0.75 * wave), 0, 100);

  return rgbToHex(hslToRgb({ h, s, l }));
}
