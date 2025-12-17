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

namespace ui::external_app::spectrum_analyzer {

class SpectrumFFTView : public View {
   public:
    SpectrumFFTView(const Rect parent_rect);

    SpectrumFFTView(const SpectrumFFTView&) = delete;
    SpectrumFFTView& operator=(const SpectrumFFTView&) = delete;

    void paint(Painter& painter) override;

    void on_channel_spectrum(const ChannelSpectrum& spectrum);

    void set_peak_hold(bool enabled);
    void clear_peak_hold();

   private:
    static constexpr size_t spectrum_size = 256;
    static constexpr size_t display_bins = 240;  // screen_width

    int16_t spectrum_data[display_bins]{0};
    int16_t peak_hold_data[display_bins]{0};
    bool peak_hold_enabled{false};

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

    void on_hide() override;
    void set_parent_rect(const Rect new_parent_rect) override;
    void focus() override;

    std::string title() const override { return "Spectrum Analyzer"; };

   private:
    static constexpr ui::Dim header_height = 3 * 16;
    static constexpr ui::Dim fft_height = 2 * 16;
    static constexpr ui::Dim scale_height = 20;
    static constexpr uint32_t bandwidth_hz = 20000000;  // 20 MHz fixed bandwidth
    static constexpr Dim waterfall_start_y = header_height + fft_height + scale_height;

    NavigationView& nav_;
    RxRadioState radio_state_{};
    app_settings::SettingsManager settings_{
        "spectrum_analyzer", app_settings::Mode::RX};

    RSSI rssi{
        {21 * 8, 0, 6 * 8, 4}};

    Channel channel{
        {21 * 8, 5, 6 * 8, 4}};

    FrequencyField field_freq_start{
        {5 * 8, UI_POS_Y(0)}};

    FrequencyField field_freq_end{
        {15 * 8, UI_POS_Y(0)}};

    LNAGainField field_lna{
        {5 * 8, UI_POS_Y(1)}};

    VGAGainField field_vga{
        {8 * 8, UI_POS_Y(1)}};

    RFAmpField field_rf_amp{
        {11 * 8, UI_POS_Y(1)}};

    OptionsField options_peak_hold{
        {18 * 8, UI_POS_Y(1)},
        2,
        {
            {"OFF", 0},
            {"ON ", 1},
        }};

    SpectrumFFTView fft_view{{0, header_height, screen_width, fft_height}};
    spectrum::WaterfallWidget waterfall_widget{};
    spectrum::FrequencyScale frequency_scale{};

    ChannelSpectrumFIFO* channel_fifo{nullptr};

    void on_frequency_changed();
    void on_gain_changed();
    void update_receiver();
    void on_channel_spectrum(const ChannelSpectrum& spectrum);

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
