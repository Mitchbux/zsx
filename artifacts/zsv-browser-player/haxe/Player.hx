package;

import js.Browser;
import js.Syntax;
import js.html.Element;
import js.html.Event;

/** The complete browser controller. React only supplies semantic mount points. */
class Player {
  static var root:Dynamic;
  static var worker:Dynamic;
  static var ctx:Dynamic;
  static var gain:Dynamic;
  static var sources:Array<Dynamic> = [];
  static var frames:Array<Dynamic> = [];
  static var audio:Array<Dynamic> = [];
  // Validated PCM is scheduled up front on Web Audio's rendering thread.
  // It must not depend on video-worker replies or main-thread redraw timers.
  static var audioTrack:Array<Dynamic> = [];
  static var independentAudio = false;
  static var playing = false;
  static var running = false;
  static var pending = false;
  static var eof = false;
  static var epoch = 0;
  static var videoThrough:Float = 0;
  static var audioThrough:Float = 0;
  static var scheduledUntil:Float = 0;
  static var queuedBytes:Float = 0;
  static var queueLimit = 192 * 1024 * 1024;
  static var batchLimit = 4 * 1024 * 1024;
  static var timer = 0;
  static var drawn:Dynamic;
  static var media:Dynamic;
  static var offset:Float = 0;
  static var started:Float = 0;
  static var muted = false;
  static var volume:Float = .8;
  static var limit = 64 * 1024 * 1024;
  static var generation = 0;

  static function el(id:String):Dynamic return Browser.document.getElementById(id);
  static function text(id:String, value:String) el(id).textContent = value;
  static function seconds(v:Float):String {
    var n = Std.int(v); return StringTools.lpad(Std.string(Std.int(n / 60)), "0", 2) + ":" + StringTools.lpad(Std.string(n % 60), "0", 2);
  }
  static function state(name:String, message:String):Void { text("zsv-status-text", message); root.dataset.status = name; }
  static function playLabel(playing:Bool):Void {
    el("zsv-play").textContent = playing ? "❚❚" : "▶";
    el("zsv-play").setAttribute("aria-label", playing ? "Pause" : "Play");
  }
  static function fail(message:String):Void { generation++; cleanup(); media = null; state("error", message); el("zsv-empty").classList.remove("hidden"); el("zsv-video").classList.add("hidden"); el("zsv-controls").classList.add("opacity-50"); el("zsv-controls").classList.add("pointer-events-none"); }
  static function cleanup():Void {
    if (worker != null) worker.terminate();
    worker = null;
    stopSources();
    playing = false; running = false; pending = false; eof = false;
    frames = []; audio = []; audioTrack = []; independentAudio = false; queuedBytes = 0; drawn = null;
    root.dataset.queuedBytes = "0"; root.dataset.queueItems = "0";
    Browser.window.clearInterval(timer);
    if (ctx != null) try ctx.close() catch (_) {};
    ctx = null; gain = null; offset = 0; epoch++;
    playLabel(false);
  }
  static function beginLoad():Int {
    generation++;
    cleanup();
    media = null;
    state("loading", "READING...");
    el("zsv-controls").classList.add("opacity-50");
    el("zsv-controls").classList.add("pointer-events-none");
    return generation;
  }
  static function decode(bytes:Dynamic, name:String, token:Int, ?stream:Dynamic):Void {
    if (token != generation) return;
    if (bytes != null && bytes.byteLength > limit) { fail("File exceeds the 64 MiB encoded input limit."); return; }
    cleanup(); media = null; state("loading", "OPENING STREAM...");
    var url = el("zsv-worker-url").dataset.workerUrl;
    worker = Syntax.code("new Worker({0}, {type:'module'})", url);
    worker.onmessage = function(e:Dynamic) {
      if (token != generation) return;
      if (e.data.type == "error") fail(e.data.message);
      else if (e.data.type == "metadata") {
        media = e.data;
        if (stream != null && stream.duration != null) {
          media.duration = stream.duration; media.durationKnown = true;
        }
        independentAudio = e.data.audioTrack != null;
        audioTrack = independentAudio ? cast e.data.audioTrack : [];
        root.dataset.audioMode = independentAudio ? "independent-render-thread" : "streaming";
        mountReady(); seek(0); playLabel(playing);
        timer = Browser.window.setInterval(function() tick(0), 25);
      } else if (e.data.type == "batch" && e.data.epoch == epoch) {
        try receive(e.data) catch (_) fail("Unable to allocate or schedule browser playback.");
      }
    };
    worker.onerror = function(_) if (token == generation) fail("Decoder worker failed to load.");
    if (stream != null) {
      // Resume during the Load Sample/file-picker gesture, not after buffering.
      if (stream.autoplay == true) {
        ctx = Syntax.code("new (window.AudioContext || window.webkitAudioContext)()");
        gain = ctx.createGain(); gain.gain.value = muted ? 0 : volume; gain.connect(ctx.destination);
        playing = true;
        ctx.resume().then(function(_) {}, function(_) {
          if (token == generation) { playing = false; playLabel(false); }
        });
      }
      worker.postMessage(stream);
    }
    else worker.postMessage({type:"decode", bytes:bytes, name:name}, [bytes]);
  }
  static function mountReady():Void {
    el("zsv-empty").classList.add("hidden"); el("zsv-video").classList.remove("hidden"); el("zsv-video").classList.add("flex");
    el("zsv-controls").classList.remove("opacity-50"); el("zsv-controls").classList.remove("pointer-events-none");
    text("zsv-duration", media.durationKnown == false ? "STREAM" : seconds(media.duration)); text("zsv-elapsed", "00:00");
    el("zsv-seek").max = media.durationKnown == false ? 0 : media.duration; el("zsv-seek").value = 0;
    el("zsv-drop-zone").dataset.mediaName = media.name;
    el("zsv-drop-zone").dataset.fps = media.fps.toFixed(2);
    playLabel(false);
    draw(0);
  }
  static function draw(position:Float):Void {
    if (media == null) return;
    var canvas:Dynamic = el("zsv-canvas");
    while (frames.length > 1 && frames[1].time <= position) {
      queuedBytes -= frames.shift().data.byteLength;
    }
    if (frames.length == 0 || position < frames[0].time) {
      canvas.width = media.width;
      canvas.height = media.height;
      var blank = canvas.getContext("2d");
      blank.fillStyle = "#0a0a0c";
      blank.fillRect(0, 0, canvas.width, canvas.height);
      return;
    }
    var frame = frames[0];
    if (frame == drawn) return;
    drawn = frame;
    canvas.width = frame.width; canvas.height = frame.height;
    var ctx2 = canvas.getContext("2d"), yuv = new js.lib.Uint8Array(frame.data), image = ctx2.createImageData(frame.width, frame.height), ys = frame.width * frame.height;
    for (y in 0...frame.height) for (x in 0...frame.width) {
      var yv = yuv[y * frame.width + x], u = frame.grayscale == true ? 0 : yuv[ys + (y >> 1) * (frame.width >> 1) + (x >> 1)] - 128, v = frame.grayscale == true ? 0 : yuv[ys + Std.int(ys / 4) + (y >> 1) * (frame.width >> 1) + (x >> 1)] - 128, p = (y * frame.width + x) * 4;
      image.data[p] = Math.max(0, Math.min(255, yv + 1.402 * v)); image.data[p + 1] = Math.max(0, Math.min(255, yv - .344 * u - .714 * v)); image.data[p + 2] = Math.max(0, Math.min(255, yv + 1.772 * u)); image.data[p + 3] = 255;
    }
    ctx2.putImageData(image, 0, 0);
  }
  static function tick(_:Float):Void {
    if (media == null) return;
    if (running) {
      var p = Math.min(through(), Math.max(offset, offset + ctx.currentTime - started));
      text("zsv-elapsed", seconds(p)); el("zsv-seek").value = p; prune(p);
      if (p >= media.duration) {
        offset = media.duration; stopSources(); playing = false; playLabel(false); state("ended", "ENDED"); return;
      }
      if (p >= through()) { offset = p; stopSources(); state("buffering", "BUFFERING..."); }
    }
    fill();
  }
  static function through():Float return independentAudio ? media.duration : Math.min(media.duration, media.streaming == true ? audioThrough : Math.min(videoThrough, audioThrough));
  static function buffered():Bool return eof || Math.min(videoThrough, audioThrough) - offset >= Math.min(30, media.duration - offset);
  static function position():Float return running ? Math.min(through(), Math.max(offset, offset + ctx.currentTime - started)) : offset;
  static function stopSources():Void {
    for (s in sources) { s.onended = null; try s.stop() catch (_) {}; s.disconnect(); }
    sources = []; running = false;
  }
  static function prune(p:Float):Void {
    draw(p);
    while (audio.length > 0 && audio[0].time + audio[0].samples / audio[0].rate <= p) {
      queuedBytes -= audio.shift().data.byteLength;
    }
    root.dataset.queuedBytes = Std.string(queuedBytes);
    root.dataset.queueItems = Std.string(frames.length + audio.length);
  }
  static function fill():Void {
    if (media == null || worker == null || pending || eof) return;
    if ((media.streaming == true ? Math.min(videoThrough, audioThrough) : independentAudio ? videoThrough : through()) - position() >= (media.streaming == true ? 30 : 1.5)) return;
    // Reserve one entire transferred batch before granting the worker credit.
    if (queuedBytes > queueLimit - batchLimit || frames.length + audio.length > 8000) {
      if (!running && (media.streaming == true || through() <= offset + .1)) fail("30-second buffer exceeds the 192 MiB playback limit. Use a lower resolution.");
      return;
    }
    pending = true; worker.postMessage({type:"pull", epoch:epoch});
  }
  static function receive(batch:Dynamic):Void {
    // Freeze at the old coverage before extending it: late worker messages
    // must never advance the clock over audio that was not scheduled.
    tick(0);
    if (media == null) return;
    pending = false;
    for (f in (cast batch.frames:Array<Dynamic>)) { frames.push(f); queuedBytes += f.data.byteLength; }
    if (!independentAudio) for (a in (cast batch.audio:Array<Dynamic>)) { audio.push(a); queuedBytes += a.data.byteLength; }
    if (queuedBytes > queueLimit || frames.length + audio.length > 8192) { fail("Stream exceeds the bounded playback queue."); return; }
    videoThrough = batch.videoThrough; audioThrough = batch.audioThrough; eof = batch.eof;
    if (media.streaming == true) {
      if (batch.durationKnown == true) { media.duration = batch.duration; media.durationKnown = true; }
      el("zsv-seek").max = media.durationKnown == true ? media.duration : Math.max(0, Math.min(videoThrough, audioThrough));
      text("zsv-duration", media.durationKnown == true ? seconds(media.duration) : "STREAM");
      root.dataset.bufferedSeconds = Std.string(Math.max(0, Math.min(videoThrough, audioThrough) - position()));
      if (!running) state("buffering", "BUFFERING " + Std.int(Math.max(0, Math.min(videoThrough, audioThrough) - offset)) + " / 30s");
    }
    prune(position());
    if (!running && (media.streaming == true ? buffered() : (through() > offset + .1 || eof))) {
      state("ready", "READY");
      if (playing) startRun();
    } else if (running) schedule();
    fill();
  }
  static function schedule():Void {
    if (!running) return;
    var end = through();
    var deferred:Array<Dynamic> = [];
    for (a in (independentAudio ? audioTrack : audio)) {
      var from:Float = Math.max(Math.max(offset, scheduledUntil), a.time);
      var until:Float = Math.min(end, a.time + a.samples / a.rate);
      if (until <= from) continue;
      var first = Std.int(Math.round((from - a.time) * a.rate));
      var count = Std.int(Math.round((until - a.time) * a.rate)) - first;
      if (count <= 0) continue;
      var samples = new js.lib.Float32Array(a.data), channels:Int = a.channels;
      var buffer = ctx.createBuffer(channels, count, a.rate);
      for (channel in 0...channels) {
        var part = buffer.getChannelData(channel);
        for (i in 0...count) part[i] = samples[(i + first) * channels + channel];
      }
      var s = ctx.createBufferSource(); s.buffer = buffer; s.connect(gain);
      sources.push(s);
      s.onended = function() { sources.remove(s); s.disconnect(); };
      if (independentAudio || (media.streaming == true && scheduledUntil == offset)) deferred.push({source:s, from:from});
      else s.start(started + from - offset);
    }
    if (deferred.length > 0) {
      started = ctx.currentTime + .03;
      for (entry in deferred) entry.source.start(started + entry.from - offset);
    }
    scheduledUntil = end;
  }
  static function startRun():Void {
    if (running || !playing || ctx == null) return;
    if (offset >= media.duration) { playing = false; playLabel(false); state("ended", "ENDED"); return; }
    if ((media.streaming == true ? !buffered() : through() <= offset + .1 && !eof)) { state("buffering", "BUFFERING..."); fill(); return; }
    // schedule() starts independent audio only after all buffers are prepared.
    started = ctx.currentTime + .03; scheduledUntil = offset; running = true;
    schedule(); state("ready", "READY");
  }
  static function play():Void {
    if (media == null) return;
    if (ctx == null) { ctx = Syntax.code("new (window.AudioContext || window.webkitAudioContext)()"); gain = ctx.createGain(); gain.gain.value = muted ? 0 : volume; gain.connect(ctx.destination); }
    var token = generation;
    playing = true; playLabel(true);
    ctx.resume().then(function(_) {
      if (token != generation || !playing) return;
      try {
        if (offset >= media.duration) seek(0) else startRun();
      } catch (_) fail("Unable to allocate or schedule browser audio.");
    }, function(_) if (token == generation) fail("Unable to start browser audio."));
  }
  static function pause():Void {
    offset = position(); stopSources(); playing = false; playLabel(false);
    text("zsv-elapsed", seconds(offset)); el("zsv-seek").value = offset; prune(offset);
  }
  static function seek(v:Float):Void {
    if (media == null) return;
    stopSources(); epoch++; frames = []; audio = []; drawn = null; queuedBytes = 0;
    offset = Math.max(0, Math.min(media.duration, v)); videoThrough = offset; audioThrough = offset; eof = false;
    text("zsv-elapsed", seconds(offset)); el("zsv-seek").value = offset; draw(offset);
    state("buffering", "SEEKING..."); pending = true;
    worker.postMessage({type:"seek", epoch:epoch, time:offset});
  }
  static function loadFile(file:Dynamic):Void {
    if (file == null) return;
    var token = beginLoad();
    if (file.size > limit) { fail("File exceeds the 64 MiB encoded input limit."); return; }
    if (Syntax.code("typeof {0}.stream === 'function'", file)) {
      decode(null, file.name, token, {type:"stream", file:file, name:file.name}); return;
    }
    file.arrayBuffer().then(
      function(b) if (token == generation) decode(b, file.name, token),
      function(_) if (token == generation) fail("Unable to read the selected file.")
    );
  }
  static function mount():Void {
    root = el("zsv-drop-zone"); if (root == null) { Browser.window.setTimeout(mount, 0); return; }
    el("zsv-open").onclick = function(_) el("zsv-file").click(); el("zsv-file").onchange = function(_) loadFile(el("zsv-file").files[0]);
    el("zsv-sample").onclick = function(_) {
      var token = beginLoad();
      decode(null, "snowboard-full-grayscale-25fps.zsv", token, {type:"stream", url:el("zsv-worker-url").dataset.sampleUrl, name:"snowboard-full-grayscale-25fps.zsv", duration:271.627029, autoplay:true});
    };
    root.ondragover = function(e) { e.preventDefault(); }; root.ondrop = function(e) { e.preventDefault(); loadFile(e.dataTransfer.files[0]); };
    el("zsv-play").onclick = function(_) if (!playing) play() else pause();
    el("zsv-mute").onclick = function(_) { muted = !muted; root.dataset.muted = muted ? "true" : "false"; if (gain != null) gain.gain.value = muted ? 0 : volume; el("zsv-mute").setAttribute("aria-label", muted ? "Unmute" : "Mute"); };
    el("zsv-volume").oninput = function(_) { volume = Std.parseFloat(el("zsv-volume").value) / 100; root.dataset.volume = Std.string(volume); muted = false; root.dataset.muted = "false"; if (gain != null) gain.gain.value = volume; };
    el("zsv-seek").oninput = function(_) seek(Std.parseFloat(el("zsv-seek").value));
  }
  public static function main():Void mount();
}