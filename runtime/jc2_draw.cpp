// Jade Cocoon 2's pictures drawn natively (BozoEngine's renderer): no GS, no GIF or VIF packet read. Each family of the
// game's draws is taken from its own state at the routine that would build its packets.
//   Movies: the pictures BozoEngine's libmpeg decodes (FFmpeg) shown as the renderer's backdrop.
#include "bozo/port.h"
#include "bozo/render/movie.h"
#include "bozo/render/renderer.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

namespace jc2
{
    namespace
    {
        // JC2's display: 640 x 448 (NTSC, a frame of two fields), centred on the GS's 2048 origin.
        bozo::render::Screen screen()
        {
            bozo::render::Screen s;
            s.originX = 2048.0f - 320.0f;
            s.originY = 2048.0f - 224.0f;
            s.width = 640.0f;
            s.height = 448.0f;
            return s;
        }

        std::mutex movieMutex;
        std::shared_ptr<const bozo::render::Texture> moviePicture;
        uint64_t movieVersion = 0;

        // sceMpegGetPicture's picture (decoder thread or game thread): kept for the next frame end.
        void onMoviePicture(uint32_t, uint32_t width, uint32_t height, const uint8_t *rgba)
        {
            auto texture = std::make_shared<bozo::render::Texture>();
            texture->width = width;
            texture->height = height;
            texture->rgba.resize(size_t(width) * height);
            std::memcpy(texture->rgba.data(), rgba, texture->rgba.size() * 4);
            for (uint32_t &p : texture->rgba)
                p |= 0xFF000000u; // a movie picture is opaque
            texture->version = ++movieVersion;
            std::lock_guard<std::mutex> lock(movieMutex);
            moviePicture = std::move(texture);
        }
    } // namespace

    // The game's frame ended (the frame wait): what was drawn for it is published, the next frame begins.
    void drawFrameEnd()
    {
        auto &renderer = bozo::render::Renderer::instance();
        std::shared_ptr<const bozo::render::Texture> picture;
        {
            std::lock_guard<std::mutex> lock(movieMutex);
            picture = std::move(moviePicture);
        }
        const bozo::render::Screen s = screen();
        if (picture)
            renderer.setBackdrop(picture, s, s.originX, s.originY, s.width, s.height, true);
        renderer.publishOverlay();
        renderer.endFrame();
        renderer.beginFrame(s);
    }

    void installDraw(Runtime &, bozo::port::Registry &)
    {
        auto &renderer = bozo::render::Renderer::instance();
        renderer.setEnabled(true);
        renderer.setStreamDraws(false);
        renderer.setPresentsAlone(true);
        renderer.beginFrame(screen());
        bozo::render::setMoviePictureHook(&onMoviePicture);
    }
} // namespace jc2
