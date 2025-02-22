/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "tools/viewer/SkottieSlide.h"

#if defined(SK_ENABLE_SKOTTIE)

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkTime.h"
#include "include/private/SkNoncopyable.h"
#include "include/private/SkTPin.h"
#include "modules/audioplayer/SkAudioPlayer.h"
#include "modules/skottie/include/Skottie.h"
#include "modules/skottie/include/SkottieProperty.h"
#include "modules/skottie/utils/SkottieUtils.h"
#include "modules/skresources/include/SkResources.h"
#include "src/utils/SkOSPath.h"
#include "tools/timer/TimeUtils.h"

#include <cmath>
#include <vector>

#include "imgui.h"

namespace {

class Track final : public skresources::ExternalTrackAsset {
public:
    explicit Track(std::unique_ptr<SkAudioPlayer> player) : fPlayer(std::move(player)) {}

private:
    void seek(float t) override {
        if (fPlayer->isStopped() && t >=0) {
            fPlayer->play();
        }

        if (fPlayer->isPlaying()) {
            if (t < 0) {
                fPlayer->stop();
            } else {
                static constexpr float kTolerance = 0.075f;
                const auto player_pos = fPlayer->time();

                if (std::abs(player_pos - t) > kTolerance) {
                    fPlayer->setTime(t);
                }
            }
        }
    }

    const std::unique_ptr<SkAudioPlayer> fPlayer;
};

class AudioProviderProxy final : public skresources::ResourceProviderProxyBase {
public:
    explicit AudioProviderProxy(sk_sp<skresources::ResourceProvider> rp)
        : INHERITED(std::move(rp)) {}

private:
    sk_sp<skresources::ExternalTrackAsset> loadAudioAsset(const char path[],
                                                          const char name[],
                                                          const char[] /*id*/) override {
        if (auto data = this->load(path, name)) {
            if (auto player = SkAudioPlayer::Make(std::move(data))) {
                return sk_make_sp<Track>(std::move(player));
            }
        }

        return nullptr;
    }

    using INHERITED = skresources::ResourceProviderProxyBase;
};

class Decorator : public SkNoncopyable {
public:
    virtual ~Decorator() = default;

    virtual void render(SkCanvas*) = 0;
};

class SimpleMarker final : public Decorator {
public:
    ~SimpleMarker() override = default;

    static std::unique_ptr<Decorator> Make() { return std::make_unique<SimpleMarker>(); }

    void render(SkCanvas* canvas) override {
        SkPaint p;
        p.setAntiAlias(true);

        p.setColor(SK_ColorGREEN);
        canvas->drawCircle(0, 0, 5, p);

        p.setColor(SK_ColorRED);
        p.setStrokeWidth(1.5f);
        canvas->drawLine(-10, 0, 10, 0, p);
        canvas->drawLine(0, -10, 0, 10, p);
    }
};

static const struct DecoratorRec {
    const char* fName;
    std::unique_ptr<Decorator>(*fFactory)();
} kDecorators[] = {
    { "Simple marker", SimpleMarker::Make }
};

} // namespace

class SkottieSlide::TransformTracker : public skottie::PropertyObserver {
public:
    void renderUI() {
        if (ImGui::Begin("Transform Tracker", nullptr)) {
            if (ImGui::BeginCombo("Transform", fTransformSelect
                                       ? std::get<0>(*fTransformSelect).c_str()
                                       : nullptr)) {
                if (ImGui::Selectable("(none)", true)) {
                    fTransformSelect = nullptr;
                }
                for (const auto& entry : fTransforms) {
                    const auto* transform_name = std::get<0>(entry).c_str();
                    if (ImGui::Selectable(transform_name, false)) {
                        if (!fTransformSelect ||
                            transform_name != std::get<0>(*fTransformSelect).c_str()) {
                            fTransformSelect = &entry;
                            // Reset the decorator on transform change.
                            fDecorator = fDecoratorSelect->fFactory();
                        }
                    }
                }
                ImGui::EndCombo();
            }

            if (ImGui::BeginCombo("Decoration", fDecoratorSelect->fName)) {
                for (const auto& dec : kDecorators) {
                    if (ImGui::Selectable(dec.fName, false)) {
                        if (dec.fName != fDecoratorSelect->fName) {
                            fDecoratorSelect = &dec;
                            fDecorator = fDecoratorSelect->fFactory();
                        }
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::End();
    }

    void renderTracker(SkCanvas* canvas, const SkSize& win_size, const SkSize& anim_size) const {
        if (!fTransformSelect) {
            return;
        }

        const auto tprop = std::get<1>(*fTransformSelect)->get();

        const auto m = SkMatrix::Translate(tprop.fPosition.fX, tprop.fPosition.fY)
                     * SkMatrix::RotateDeg(tprop.fRotation)
                     * SkMatrix::Scale    (tprop.fScale.fX*0.01f, tprop.fScale.fY*0.01f)
                     * SkMatrix::Translate(tprop.fAnchorPoint.fX, tprop.fAnchorPoint.fY);

        const auto viewer_matrix = SkMatrix::RectToRect(SkRect::MakeSize(anim_size),
                                                        SkRect::MakeSize(win_size),
                                                        SkMatrix::kCenter_ScaleToFit);

        SkAutoCanvasRestore acr(canvas, true);
        canvas->concat(viewer_matrix);
        canvas->concat(m);

        SkASSERT(fDecorator);
        fDecorator->render(canvas);
    }

private:
    void onTransformProperty(const char name[],
                             const LazyHandle<skottie::TransformPropertyHandle>& lh) override {

        fTransforms.push_back(std::make_tuple(SkString(name), lh()));
    }

    using TransformT = std::tuple<SkString, std::unique_ptr<skottie::TransformPropertyHandle>>;

    std::vector<TransformT>    fTransforms;
    std::unique_ptr<Decorator> fDecorator;
    const TransformT*          fTransformSelect = nullptr;
    const DecoratorRec*        fDecoratorSelect = &kDecorators[0];
};

static void draw_stats_box(SkCanvas* canvas, const skottie::Animation::Builder::Stats& stats) {
    static constexpr SkRect kR = { 10, 10, 280, 120 };
    static constexpr SkScalar kTextSize = 20;

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(0xffeeeeee);

    SkFont font(nullptr, kTextSize);

    canvas->drawRect(kR, paint);

    paint.setColor(SK_ColorBLACK);

    const auto json_size = SkStringPrintf("Json size: %zu bytes",
                                          stats.fJsonSize);
    canvas->drawString(json_size, kR.x() + 10, kR.y() + kTextSize * 1, font, paint);
    const auto animator_count = SkStringPrintf("Animator count: %zu",
                                               stats.fAnimatorCount);
    canvas->drawString(animator_count, kR.x() + 10, kR.y() + kTextSize * 2, font, paint);
    const auto json_parse_time = SkStringPrintf("Json parse time: %.3f ms",
                                                stats.fJsonParseTimeMS);
    canvas->drawString(json_parse_time, kR.x() + 10, kR.y() + kTextSize * 3, font, paint);
    const auto scene_parse_time = SkStringPrintf("Scene build time: %.3f ms",
                                                 stats.fSceneParseTimeMS);
    canvas->drawString(scene_parse_time, kR.x() + 10, kR.y() + kTextSize * 4, font, paint);
    const auto total_load_time = SkStringPrintf("Total load time: %.3f ms",
                                                stats.fTotalLoadTimeMS);
    canvas->drawString(total_load_time, kR.x() + 10, kR.y() + kTextSize * 5, font, paint);

    paint.setStyle(SkPaint::kStroke_Style);
    canvas->drawRect(kR, paint);
}

SkottieSlide::SkottieSlide(const SkString& name, const SkString& path)
    : fPath(path) {
    fName = name;
}

void SkottieSlide::load(SkScalar w, SkScalar h) {
    class Logger final : public skottie::Logger {
    public:
        struct LogEntry {
            SkString fMessage,
                     fJSON;
        };

        void log(skottie::Logger::Level lvl, const char message[], const char json[]) override {
            auto& log = lvl == skottie::Logger::Level::kError ? fErrors : fWarnings;
            log.push_back({ SkString(message), json ? SkString(json) : SkString() });
        }

        void report() const {
            SkDebugf("Animation loaded with %zu error%s, %zu warning%s.\n",
                     fErrors.size(), fErrors.size() == 1 ? "" : "s",
                     fWarnings.size(), fWarnings.size() == 1 ? "" : "s");

            const auto& show = [](const LogEntry& log, const char prefix[]) {
                SkDebugf("%s%s", prefix, log.fMessage.c_str());
                if (!log.fJSON.isEmpty())
                    SkDebugf(" : %s", log.fJSON.c_str());
                SkDebugf("\n");
            };

            for (const auto& err : fErrors)   show(err, "  !! ");
            for (const auto& wrn : fWarnings) show(wrn, "  ?? ");
        }

    private:
        std::vector<LogEntry> fErrors,
                              fWarnings;
    };

    auto logger = sk_make_sp<Logger>();

    uint32_t flags = 0;
    if (fPreferGlyphPaths) {
        flags |= skottie::Animation::Builder::kPreferEmbeddedFonts;
    }
    skottie::Animation::Builder builder(flags);

    auto resource_provider =
        sk_make_sp<AudioProviderProxy>(
            skresources::DataURIResourceProviderProxy::Make(
                skresources::FileResourceProvider::Make(SkOSPath::Dirname(fPath.c_str()),
                                                        /*predecode=*/true),
                /*predecode=*/true));

    static constexpr char kInterceptPrefix[] = "__";
    auto precomp_interceptor =
            sk_make_sp<skottie_utils::ExternalAnimationPrecompInterceptor>(resource_provider,
                                                                           kInterceptPrefix);

    fTransformTracker = sk_make_sp<TransformTracker>();

    fAnimation      = builder
            .setLogger(logger)
            .setResourceProvider(std::move(resource_provider))
            .setPrecompInterceptor(std::move(precomp_interceptor))
            .setPropertyObserver(fTransformTracker)
            .makeFromFile(fPath.c_str());
    fAnimationStats = builder.getStats();
    fWinSize        = SkSize::Make(w, h);
    fTimeBase       = 0; // force a time reset

    if (fAnimation) {
        fAnimation->seek(0);
        fFrameTimes.resize(SkScalarCeilToInt(fAnimation->duration() * fAnimation->fps()));
        SkDebugf("Loaded Bodymovin animation v: %s, size: [%f %f]\n",
                 fAnimation->version().c_str(),
                 fAnimation->size().width(),
                 fAnimation->size().height());
        logger->report();
    } else {
        SkDebugf("failed to load Bodymovin animation: %s\n", fPath.c_str());
    }
}

void SkottieSlide::unload() {
    fAnimation.reset();
}

void SkottieSlide::resize(SkScalar w, SkScalar h) {
    fWinSize = { w, h };
}

SkISize SkottieSlide::getDimensions() const {
    // We always scale to fill the window.
    return fWinSize.toCeil();
}

void SkottieSlide::draw(SkCanvas* canvas) {
    if (fAnimation) {
        SkAutoCanvasRestore acr(canvas, true);
        const auto dstR = SkRect::MakeSize(fWinSize);

        {
            const auto t0 = SkTime::GetNSecs();
            fAnimation->render(canvas, &dstR);

            // TODO: this does not capture GPU flush time!
            const auto  frame_index  = static_cast<size_t>(fCurrentFrame);
            fFrameTimes[frame_index] = static_cast<float>((SkTime::GetNSecs() - t0) * 1e-6);
        }

        fTransformTracker->renderTracker(canvas, fWinSize, fAnimation->size());

        if (fShowAnimationStats) {
            draw_stats_box(canvas, fAnimationStats);
        }
        if (fShowAnimationInval) {
            const auto t = SkMatrix::RectToRect(SkRect::MakeSize(fAnimation->size()), dstR,
                                                SkMatrix::kCenter_ScaleToFit);
            SkPaint fill, stroke;
            fill.setAntiAlias(true);
            fill.setColor(0x40ff0000);
            stroke.setAntiAlias(true);
            stroke.setColor(0xffff0000);
            stroke.setStyle(SkPaint::kStroke_Style);

            for (const auto& r : fInvalController) {
                SkRect bounds;
                t.mapRect(&bounds, r);
                canvas->drawRect(bounds, fill);
                canvas->drawRect(bounds, stroke);
            }
        }
        if (fShowUI) {
            this->renderUI();
        }
        if (fShowTrackerUI) {
            fTransformTracker->renderUI();
        }
    }
}

bool SkottieSlide::animate(double nanos) {
    if (!fTimeBase) {
        // Reset the animation time.
        fTimeBase = nanos;
    }

    if (fAnimation) {
        fInvalController.reset();

        const auto frame_count = fAnimation->duration() * fAnimation->fps();

        if (!fDraggingProgress) {
            // Clock-driven progress: update current frame.
            const double t_sec = (nanos - fTimeBase) * 1e-9;
            fCurrentFrame = std::fmod(t_sec * fAnimation->fps(), frame_count);
        } else {
            // Slider-driven progress: update the time origin.
            fTimeBase = nanos - fCurrentFrame / fAnimation->fps() * 1e9;
        }

        // Sanitize and rate-lock the current frame.
        fCurrentFrame = SkTPin<float>(fCurrentFrame, 0.0f, frame_count - 1);
        if (fFrameRate > 0) {
            const auto fps_scale = fFrameRate / fAnimation->fps();
            fCurrentFrame = std::trunc(fCurrentFrame * fps_scale) / fps_scale;
        }

        fAnimation->seekFrame(fCurrentFrame, fShowAnimationInval ? &fInvalController
                                                                 : nullptr);
    }
    return true;
}

bool SkottieSlide::onChar(SkUnichar c) {
    switch (c) {
    case 'I':
        fShowAnimationStats = !fShowAnimationStats;
        return true;
    case 'G':
        fPreferGlyphPaths = !fPreferGlyphPaths;
        this->load(fWinSize.width(), fWinSize.height());
        return true;
    case 'T':
        fShowTrackerUI = !fShowTrackerUI;
        return true;
    }

    return INHERITED::onChar(c);
}

bool SkottieSlide::onMouse(SkScalar x, SkScalar y, skui::InputState state, skui::ModifierKey) {
    switch (state) {
    case skui::InputState::kUp:
        fShowAnimationInval = !fShowAnimationInval;
        fShowAnimationStats = !fShowAnimationStats;
        break;
    default:
        break;
    }

    fShowUI = this->UIArea().contains(x, y);

    return false;
}

SkRect SkottieSlide::UIArea() const {
    static constexpr float kUIHeight = 120.0f;

    return SkRect::MakeXYWH(0, fWinSize.height() - kUIHeight, fWinSize.width(), kUIHeight);
}

void SkottieSlide::renderUI() {
    static constexpr auto kUI_opacity     = 0.35f,
                          kUI_hist_height = 50.0f,
                          kUI_fps_width   = 100.0f;

    auto add_frame_rate_option = [this](const char* label, double rate) {
        const auto is_selected = (fFrameRate == rate);
        if (ImGui::Selectable(label, is_selected)) {
            fFrameRate      = rate;
            fFrameRateLabel = label;
        }
        if (is_selected) {
            ImGui::SetItemDefaultFocus();
        }
    };

    ImGui::SetNextWindowBgAlpha(kUI_opacity);
    if (ImGui::Begin("Skottie Controls", nullptr, ImGuiWindowFlags_NoDecoration |
                                                  ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoMove |
                                                  ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoFocusOnAppearing |
                                                  ImGuiWindowFlags_NoNav)) {
        const auto ui_area = this->UIArea();
        ImGui::SetWindowPos(ImVec2(ui_area.x(), ui_area.y()));
        ImGui::SetWindowSize(ImVec2(ui_area.width(), ui_area.height()));

        ImGui::PushItemWidth(-1);
        ImGui::PlotHistogram("", fFrameTimes.data(), fFrameTimes.size(),
                                 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(0, kUI_hist_height));
        ImGui::SliderFloat("", &fCurrentFrame, 0, fAnimation->duration() * fAnimation->fps() - 1);
        fDraggingProgress = ImGui::IsItemActive();
        ImGui::PopItemWidth();

        ImGui::PushItemWidth(kUI_fps_width);
        if (ImGui::BeginCombo("FPS", fFrameRateLabel)) {
            add_frame_rate_option("", 0.0);
            add_frame_rate_option("Native", fAnimation->fps());
            add_frame_rate_option( "1",  1.0);
            add_frame_rate_option("15", 15.0);
            add_frame_rate_option("24", 24.0);
            add_frame_rate_option("30", 30.0);
            add_frame_rate_option("60", 60.0);
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
    }
    ImGui::End();
}

#endif // SK_ENABLE_SKOTTIE
