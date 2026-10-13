import React from "react";
import { Play, Volume2 } from "lucide-react";

/** Auto-hiding overlay. Haxe wires the IDs; React only manages visibility and seek fill. */
export function ControlsOverlay() {
  const ref = React.useRef<HTMLDivElement>(null);
  const [visible, setVisible] = React.useState(true);

  React.useEffect(() => {
    const host = ref.current?.parentElement;
    if (!host) return;
    let t = 0;
    const wake = () => {
      setVisible(true);
      window.clearTimeout(t);
      t = window.setTimeout(() => {
        const active = document.activeElement;
        if (active && host.contains(active) && (active as HTMLElement).id === "zsv-seek" && (active as HTMLElement).matches(":active")) return;
        setVisible(false);
      }, 2000);
    };
    const opts = { passive: true } as AddEventListenerOptions;
    host.addEventListener("pointermove", wake, opts);
    host.addEventListener("pointerdown", wake, opts);
    host.addEventListener("touchstart", wake, opts);
    window.addEventListener("keydown", wake);
    wake();
    let raf = 0;
    const seek = document.getElementById("zsv-seek") as HTMLInputElement | null;
    const loop = () => {
      if (seek) {
        const max = Number(seek.max) || 0;
        const p = max > 0 ? (Number(seek.value) / max) * 100 : 0;
        seek.style.setProperty("--p", `${Math.max(0, Math.min(100, p))}%`);
      }
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => {
      window.clearTimeout(t);
      cancelAnimationFrame(raf);
      host.removeEventListener("pointermove", wake);
      host.removeEventListener("pointerdown", wake);
      host.removeEventListener("touchstart", wake);
      window.removeEventListener("keydown", wake);
    };
  }, []);

  return (
    <div ref={ref} data-visible={visible} className="zsv-overlay absolute inset-0 pointer-events-none">
      <div id="zsv-controls" className="absolute inset-0 opacity-50 pointer-events-none">
        <button id="zsv-play" aria-label="Play" data-testid="button-play-pause" className="zsv-overlay-item zsv-play absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2 w-16 h-16 sm:w-20 sm:h-20 rounded-full bg-primary text-primary-foreground flex items-center justify-center text-xl sm:text-2xl shadow-lg"><Play /></button>
        <div className="zsv-overlay-item zsv-bar absolute inset-x-0 bottom-0 px-3 sm:px-5 pb-2 sm:pb-3 pt-10 flex flex-col gap-1 text-white">
          <input id="zsv-seek" type="range" min="0" max="1" step="0.01" defaultValue="0" aria-label="Seek" data-testid="input-seek" className="zsv-seek w-full" />
          <div className="flex items-center gap-3">
            <span className="font-mono text-[11px] sm:text-xs tabular-nums"><span id="zsv-elapsed" data-testid="text-elapsed">00:00</span><span className="opacity-60"> / </span><span id="zsv-duration" data-testid="text-duration">00:00</span></span>
            <div className="ml-auto flex items-center gap-2 w-28 sm:w-40">
              <button id="zsv-mute" aria-label="Mute" data-testid="button-volume" className="p-1.5 rounded hover:bg-white/15"><Volume2 className="w-5 h-5" /></button>
              <input id="zsv-volume" type="range" min="0" max="100" step="1" defaultValue="80" aria-label="Volume" data-testid="input-volume" className="zsv-range flex-1 min-w-0" />
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}
