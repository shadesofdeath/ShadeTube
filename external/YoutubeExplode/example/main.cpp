// Minimal test program: resolves the best audio-only stream of a video and prints its URL.
//
//   yte-example <video id or url> [--node] [--download <file>]
//   yte-example --search "<query>"
//
// --node plugs in the example Node.js engine (see NodeJsEngine.hpp) so that the "n" parameter
// of player-JS based clients is also resolved.

#include <YoutubeExplode.hpp>

#include "NodeJsEngine.hpp"

#include <cstring>
#include <iostream>
#include <memory>

using namespace YoutubeExplode;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: yte-example <video id or url> [--node] [--download <file>]\n"
                     "       yte-example --search \"<query>\"\n";
        return 2;
    }

    YoutubeClientOptions options;
    std::string download;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--node") == 0)
            options.jsEngine = std::make_shared<NodeJsEngine>();
        else if (std::strcmp(argv[i], "--download") == 0 && i + 1 < argc)
            download = argv[++i];
    }
    YoutubeClient youtube(options);

    try {
        if (std::strcmp(argv[1], "--search") == 0) {
            if (argc < 3)
                return 2;
            for (const auto& v : youtube.search().getVideos(argv[2], 10)) {
                std::cout << v.id() << "  " << (v.duration() ? formatDuration(*v.duration()) : "live") << "  "
                          << v.title() << "  (" << v.author().channelTitle() << ")\n";
            }
            return 0;
        }

        const auto videoId = Videos::VideoId::parse(argv[1]);
        const auto video = youtube.videos().get(videoId);
        std::cout << "Title:    " << video.title() << "\n"
                  << "Author:   " << video.author().channelTitle() << "\n"
                  << "Duration: " << (video.duration() ? formatDuration(*video.duration()) : "live") << "\n\n";

        const auto manifest = youtube.videos().streams().getManifest(videoId);
        std::cout << "Audio-only streams:\n";
        for (const auto& s : manifest.getAudioOnlyStreams())
            std::cout << "  itag " << s->itag() << "  " << s->container().name() << "  " << s->audioCodec() << "  "
                      << s->bitrate().toString() << "  " << s->size().toString() << "\n";

        const auto best = manifest.tryGetBestAudioOnlyStream();
        if (!best) {
            std::cerr << "No audio-only stream available.\n";
            return 1;
        }
        std::cout << "\nBest audio stream (" << best->toString() << "):\n" << best->url() << "\n";

        if (!download.empty()) {
            youtube.videos().streams().download(*best, download, [](double p) {
                std::cerr << "\rDownloading... " << static_cast<int>(p * 100) << "%" << std::flush;
            });
            std::cerr << "\nSaved to " << download << "\n";
        }
        return 0;
    } catch (const Exceptions::VideoAgeRestrictedException& ex) {
        std::cerr << "Age-restricted: " << ex.what() << "\n";
    } catch (const Exceptions::VideoUnavailableException& ex) {
        std::cerr << "Unavailable: " << ex.what() << "\n";
    } catch (const Exceptions::VideoUnplayableException& ex) {
        std::cerr << "Unplayable: " << ex.what() << "\n";
    } catch (const Exceptions::CipherExtractionException& ex) {
        std::cerr << "Cipher extraction failed: " << ex.what() << "\n";
    } catch (const Exceptions::YoutubeExplodeException& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
    }
    return 1;
}
