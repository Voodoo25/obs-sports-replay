# Texto para publicar en obsproject.com/forum/resources

> Cómo publicar: entrá a https://obsproject.com/forum/resources/ →
> "Add resource". Pegá el título, la descripción, subí capturas, y en el
> campo de descarga poné el enlace a la release de GitHub
> (https://github.com/Voodoo25/obs-sports-replay/releases/latest).
> Categoría sugerida: "Plugins & Scripts".

---

## Título
Sports Replay — Low-memory instant replay for sports broadcasts

## Descripción corta (tagline)
Instant replay with a compressed GPU buffer, slow motion, sponsor bumpers
and a replay bin dock — designed for live sports streaming.

## Descripción larga

**Sports Replay** brings TV-style instant replay to OBS without eating your
RAM. Instead of storing raw frames in memory (which can reach several GB per
camera), it keeps the buffer **compressed with your GPU** (NVENC / AMF / QSV,
x264 fallback). A 15-second 1080p60 buffer uses tens of megabytes instead of
gigabytes, so multi-camera replay setups run comfortably on a normal PC.

**Highlights**

- 🎥 Compressed replay buffer — per-camera capture filter (duration & quality
  configurable)
- ▶️ Auto-play on scene switch — cut to the replay scene and it plays the last
  N seconds automatically
- 🐢 Slow motion (10–400%) and reverse, with hotkeys
- ↩️ Configurable end action: freeze, **return to the previous scene**, or loop
- 🏷️ Sponsor bumpers — optional intro/outro clips played around the replay
- 💾 Automatic save of every replay to `.mp4` (no re-encode)
- 🗂️ **Replay bin dock** with thumbnails — double-click a saved replay to send
  it to program and return to the previous scene. Great for re-showing a play
  minutes later.
- 🎛️ Multi-camera: one filter per camera, replay any of them

**Requirements:** OBS Studio 31+ (tested on 32.0.2, Windows). A GPU hardware
H.264 encoder is recommended; falls back to x264.

**Download / source:** https://github.com/Voodoo25/obs-sports-replay
**Author:** Systec — https://www.systecinformatica.com.ar
**License:** GPL-2.0-or-later

<!-- CAPTURAS A SUBIR:
 1. El dock de repeticiones con miniaturas al lado del preview de OBS
 2. Las propiedades de la fuente Sports Replay (velocidad, acción al terminar,
    cortinas de sponsor)
 3. Una repetición en cámara lenta en programa
 4. (opcional) El filtro "Sports Replay Capture" en una cámara
-->

## Cómo usarlo (para incluir en el post)

1. Agregá el filtro **Sports Replay Capture** a cada cámara (click derecho →
   Filtros).
2. En tu escena de repe, agregá una fuente **Sports Replay** y elegí de qué
   cámara reproducir. Configurá velocidad, "acción al terminar = volver a la
   escena anterior", y (opcional) cortinas de sponsor.
3. Cortá a la escena de repe → reproduce solo y vuelve a la cámara principal.
4. Abrí el dock **Repeticiones (Sports Replay)** para ver las repes guardadas
   con miniaturas y lanzarlas con doble click.
