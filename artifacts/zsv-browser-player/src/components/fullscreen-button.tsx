import { useEffect, useState } from "react";
import { Maximize, Minimize } from "lucide-react";

export function FullscreenButton() {
  const [active, setActive] = useState(false);
  const [error, setError] = useState("");
  useEffect(() => {
    const sync = () => setActive(document.fullscreenElement?.id === "zsv-player");
    document.addEventListener("fullscreenchange", sync);
    return () => document.removeEventListener("fullscreenchange", sync);
  }, []);
  async function toggle() {
    setError("");
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else {
        const player = document.getElementById("zsv-player");
        if (!player?.requestFullscreen) throw new Error("Unsupported");
        await player.requestFullscreen();
      }
    } catch {
      setError("Fullscreen is unavailable here. Open the preview in a new tab and try again.");
    }
  }
  return <div className="absolute right-4 top-16 z-20">
    <button type="button" onClick={toggle} data-testid="button-fullscreen"
      aria-label={active ? "Exit fullscreen" : "Enter fullscreen"}
      title={active ? "Exit fullscreen (Esc)" : "Enter fullscreen"}
      className="rounded-lg border border-border bg-card p-3 text-foreground shadow-sm">
      {active ? <Minimize className="h-5 w-5" /> : <Maximize className="h-5 w-5" />}
    </button>
    {error && <p role="alert" className="absolute right-0 mt-2 w-64 rounded-lg bg-card p-3 text-sm shadow">{error}</p>}
  </div>;
}
