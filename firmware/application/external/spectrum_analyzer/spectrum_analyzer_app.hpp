/*
 * Copyright (C) 2025
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __SPECTRUM_ANALYZER_APP_H__
#define __SPECTRUM_ANALYZER_APP_H__

#include "receiver_model.hpp"
#include "ui_spectrum.hpp"
#include "ui_receiver.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "app_settings.hpp"
#include "radio_state.hpp"
#include "gradient.hpp"
#include <vector>

namespace ui::external_app::spectrum_analyzer {

class SpectrumFFTView : public View {
   public:
    SpectrumFFTView(const Rect parent_rect);

    SpectrumFFTView(const SpectrumFFTView&) = delete;
    SpectrumFFTView& operator=(const SpectrumFFTView&) = delete;

    void paint(Painter& painter) override;
    void set_parent_rect(const Rect new_parent_rect) override;

    // Returns true when a full line is complete
    bool on_channel_spectrum(const ChannelSpectrum& spectrum, 
                             rf::Frequency center_freq, 
                             rf::Frequency range_start, 
                             rf::Frequency range_end,
                             bool sweeping,
                             rf::Frequency marker_pixel_step,
                             rf::Frequency each_bin_size,
                             Gradient& gradient,
                             std::vector<Color>& waterfall_row);

    void set_peak_hold(bool enabled);
    void clear_peak_hold();
    
    // Process bins and accumulate pixels (like looking glass)
    bool process_bins(uint8_t* powerlevel, Gradient& gradient, std::vector<Color>& waterfall_row, bool fft_paused);
    void get_max_power(const ChannelSpectrum& spectrum, uint8_t bin, uint8_t& max_power, bool sweeping);

   private:
    static constexpr size_t spectrum_size = 256;
    static constexpr size_t display_bins = 240;  // screen_width

    int16_t spectrum_data[display_bins]{0};
    int16_t peak_hold_data[display_bins]{0};
    bool peak_hold_enabled{false};
    
    // Sweep accumulation state (for pixel-by-pixel accumulation like looking glass)
    rf::Frequency marker_pixel_step_{0};  // Hz per pixel in full range
    rf::Frequency each_bin_size_{0};  // Hz per spectrum bin
    rf::Frequency bins_hz_size_{0};  // Accumulated Hz coverage
    uint32_t pixel_index_{0};  // Current pixel being filled
    uint8_t max_power_{0};  // Current max power for pixel being accumulated

    Waveform waveform{
        {0, 0, screen_width, 2 * 16},
        spectrum_data,
        display_bins,
        0,
        false,
        Theme::getInstance()->bg_darkest->foreground,
        true};

    Waveform peak_hold_waveform{
        {0, 0, screen_width, 2 * 16},
        peak_hold_data,
        display_bins,
        0,
        false,
        Color::yellow(),
        false};
};

class SpectrumAnalyzerView : public View {
   public:
    SpectrumAnalyzerView(NavigationView& nav);
    ~SpectrumAnalyzerView();

    SpectrumAnalyzerView(const SpectrumAnalyzerView&) = delete;
    SpectrumAnalyzerView& operator=(const SpectrumAnalyzerView&) = delete;

    void on_show() override;
    void on_hide() override;
    void set_parent_rect(const Rect new_parent_rect) override;
    void focus() override;
    void paint(Painter& painter) override;

    std::string title() const override { return "Spectrum Analyzer"; };

   private:
    static constexpr ui::Dim header_height = 3 * 16;
    static constexpr ui::Dim fft_height = 4 * 16;
    static constexpr uint32_t bandwidth_hz = 20000000;  // 20 MHz fixed bandwidth
    
    // Sweep step calculation parameters (matching looking glass app FASTSCAN mode)
    static constexpr size_t spec_nb_bins = 256;  // Number of spectrum bins
    static constexpr size_t sweep_bin_length = 240;  // Number of bins to use per step (matches screen_width)
    static constexpr size_t sweep_ignore_dc = 4;  // Number of DC bins to ignore
    // Step size calculation: (bin_length + ignore_dc) * (bandwidth / spec_nb_bins)
    // This gives approximately 19.06 MHz step size with minimal overlap

    NavigationView& nav_;
    RxRadioState radio_state_{};
    
    // Persistent settings
    uint32_t freq_start_{0};
    uint32_t freq_end_{0};
    bool peak_hold_{false};
    
    app_settings::SettingsManager settings_{
        "spectrum_analyzer",
        app_settings::Mode::RX,
        {{"freq_start"sv, &freq_start_},
         {"freq_end"sv, &freq_end_},
         {"peak_hold"sv, &peak_hold_}}};

    RSSI rssi{
        {21 * 8, 0, 6 * 8, 4}};

    Channel channel{
        {21 * 8, 5, 6 * 8, 4}};

    Labels labels{
        {{0 * 8, UI_POS_Y(0)}, "MIN", Theme::getInstance()->fg_light->foreground},
        {{15 * 8, UI_POS_Y(0)}, "MAX", Theme::getInstance()->fg_light->foreground},
        {{0 * 8, UI_POS_Y(1)}, "LNA", Theme::getInstance()->fg_light->foreground},
        {{6 * 8, UI_POS_Y(1)}, "VGA", Theme::getInstance()->fg_light->foreground},
        {{12 * 8, UI_POS_Y(1)}, "AMP", Theme::getInstance()->fg_light->foreground},
        {{18 * 8, UI_POS_Y(1)}, "PK.", Theme::getInstance()->fg_light->foreground},
        {{0 * 8, UI_POS_Y(2)}, "MARK", Theme::getInstance()->fg_light->foreground}};

    RxFrequencyField field_freq_start{
        {4 * 8, UI_POS_Y(0)},
        nav_};

    RxFrequencyField field_freq_end{
        {19 * 8, UI_POS_Y(0)},
        nav_};

    TextField field_freq_mark{
        {5 * 8, UI_POS_Y(2), 9 * 8, 16},
        ""};

    Text text_gain{
        {19 * 8, UI_POS_Y(2), 8 * 8, 16},
        "---"};

    LNAGainField field_lna{
        {3 * 8, UI_POS_Y(1)}};

    VGAGainField field_vga{
        {9 * 8, UI_POS_Y(1)}};

    RFAmpField field_rf_amp{
        {15 * 8, UI_POS_Y(1)}};

    Checkbox checkbox_peak_hold{
        {18 * 8, UI_POS_Y(1)},
        3,
        "PK",
        true};  // small = true for same height as character

    SpectrumFFTView fft_view{{0, header_height, screen_width, fft_height}};
    spectrum::WaterfallWidget waterfall_widget{};

    ChannelSpectrumFIFO* channel_fifo{nullptr};
    ChannelSpectrum latest_spectrum{};
    rf::Frequency marker_freq_{0};  // Current marker frequency (0 means use center)
    uint8_t marker_pixel_index_{120};  // Marker pixel position (center by default, screen_width/2 = 240/2 = 120)
    uint32_t update_counter_{0};  // Counter to throttle marker/gain display updates
    
    // Waterfall accumulation state (like looking glass)
    std::vector<Color> spectrum_row_{};  // Accumulated color row for waterfall
    rf::Frequency waterfall_bins_hz_size_{0};  // Accumulated Hz for waterfall
    uint32_t waterfall_pixel_index_{0};  // Current waterfall pixel being filled

    // Sweep state variables
    rf::Frequency f_center_{0};  // Current center frequency during sweep
    rf::Frequency f_center_ini_{0};  // Initial center frequency for sweep (start of range)
    rf::Frequency f_center_end_{0};  // End center frequency for sweep (end of range)
    rf::Frequency sweep_step_{0};  // Step size for center frequency increments
    rf::Frequency freq_range_{0};  // Total frequency range (freq_end_ - freq_start_)
    bool sweeping_{false};  // Whether we're currently sweeping (range > bandwidth)
    int32_t sweep_direction_{1};  // 1 for forward, -1 for backward (for back-and-forth sweep)

    void on_frequency_changed();
    void on_marker_changed();
    void on_gain_changed();
    void update_receiver();
    void retune();  // Change center frequency during sweep
    void on_channel_spectrum(const ChannelSpectrum& spectrum);
    void update_gain_display();
    void plot_marker();

    MessageHandlerRegistration message_handler_channel_spectrum_config{
        Message::ID::ChannelSpectrumConfig,
        [this](const Message* const p) {
            const auto message = *reinterpret_cast<const ChannelSpectrumConfigMessage*>(p);
            this->channel_fifo = message.fifo;
        }};

    MessageHandlerRegistration message_handler_frame_sync{
        Message::ID::DisplayFrameSync,
        [this](const Message* const) {
            if (this->channel_fifo) {
                ChannelSpectrum channel_spectrum;
                while (channel_fifo->out(channel_spectrum)) {
                    // Pass to both FFT view and waterfall
                    this->on_channel_spectrum(channel_spectrum);
                }
            }
        }};
};

}  // namespace ui::external_app::spectrum_analyzer

#endif /*__SPECTRUM_ANALYZER_APP_H__*/
