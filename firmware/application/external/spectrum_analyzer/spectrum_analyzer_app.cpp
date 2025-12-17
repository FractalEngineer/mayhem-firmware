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

#include "spectrum_analyzer_app.hpp"

#include "baseband_api.hpp"
#include "portapack.hpp"
#include "portapack_persistent_memory.hpp"
using namespace portapack;

#include "string_format.hpp"

namespace ui::external_app::spectrum_analyzer {

/* SpectrumFFTView **********************************************************/

SpectrumFFTView::SpectrumFFTView(const Rect parent_rect)
    : View{parent_rect} {
    set_focusable(false);

    add_children({&waveform, &peak_hold_waveform});
}

void SpectrumFFTView::paint(Painter& painter) {
    const auto r = screen_rect();
    painter.fill_rectangle(r, Theme::getInstance()->bg_darkest->background);
}

void SpectrumFFTView::on_channel_spectrum(const ChannelSpectrum& spectrum) {
    // Convert spectrum to display format
    // Spectrum has 256 bins, we need 240 for display
    for (size_t i = 0; i < display_bins; i++) {
        // Map spectrum bins to display bins
        // Center the display around the middle of the spectrum
        size_t bin_idx;
        if (i < display_bins / 2) {
            bin_idx = 256 - display_bins / 2 + i;
        } else {
            bin_idx = i - display_bins / 2;
        }

        // Convert dB value to waveform value
        // spectrum.db is 0-255, convert to -128 to 127 for waveform
        int16_t value = ((int16_t)spectrum.db[bin_idx] - 127) * 256;
        spectrum_data[i] = value;

        // Update peak hold if enabled
        if (peak_hold_enabled) {
            if (value > peak_hold_data[i]) {
                peak_hold_data[i] = value;
            }
        }
    }

    waveform.set_dirty();
    if (peak_hold_enabled) {
        peak_hold_waveform.set_dirty();
    }
}

void SpectrumFFTView::set_peak_hold(bool enabled) {
    peak_hold_enabled = enabled;
    if (!enabled) {
        clear_peak_hold();
    }
}

void SpectrumFFTView::clear_peak_hold() {
    std::fill(peak_hold_data, peak_hold_data + display_bins, 0);
    peak_hold_waveform.set_dirty();
}

/* SpectrumAnalyzerView ******************************************************/

SpectrumAnalyzerView::SpectrumAnalyzerView(NavigationView& nav)
    : nav_(nav) {
    // Start baseband with wideband spectrum processor
    baseband::run_image(portapack::spi_flash::image_tag_wideband_spectrum);

    add_children({&rssi,
                  &channel,
                  &field_freq_start,
                  &field_freq_end,
                  &field_lna,
                  &field_vga,
                  &field_rf_amp,
                  &options_peak_hold,
                  &fft_view,
                  &frequency_scale,
                  &waterfall_widget});

    // Initialize frequency fields
    rf::Frequency center_freq = receiver_model.target_frequency();
    field_freq_start.set_value(center_freq - bandwidth_hz / 2);
    field_freq_end.set_value(center_freq + bandwidth_hz / 2);

    field_freq_start.on_change = [this](rf::Frequency) {
        this->on_frequency_changed();
    };

    field_freq_end.on_change = [this](rf::Frequency) {
        this->on_frequency_changed();
    };

    field_lna.on_change = [this](int32_t v) {
        receiver_model.set_lna(v);
        this->on_gain_changed();
    };

    field_vga.on_change = [this](int32_t v) {
        receiver_model.set_vga(v);
        this->on_gain_changed();
    };

    field_rf_amp.on_change = [this](int32_t v) {
        receiver_model.set_rf_amp(v > 0);
        this->on_gain_changed();
    };

    options_peak_hold.on_change = [this](size_t, OptionsField::value_t v) {
        fft_view.set_peak_hold(v > 0);
    };
    options_peak_hold.set_selected_index(0);

    frequency_scale.set_focusable(true);
    frequency_scale.on_select = [this](int32_t offset) {
        rf::Frequency center = receiver_model.target_frequency();
        receiver_model.set_target_frequency(center + offset);
        on_frequency_changed();
    };

    waterfall_widget.on_touch_select = [this](int32_t x, int32_t y) {
        if (y > screen_height - screen_height * 0.1) return;
        frequency_scale.focus();
        // Calculate frequency offset from touch position
        // This will be updated when we have sampling_rate
    };

    // Load gradient for waterfall (use default if file loading fails)
    waterfall_widget.gradient.set_default();

    // Setup receiver for 20MHz bandwidth
    update_receiver();

    // Start spectrum streaming
    baseband::spectrum_streaming_start();
    
    // Initialize waterfall widget
    waterfall_widget.on_show();
}

SpectrumAnalyzerView::~SpectrumAnalyzerView() {
    baseband::spectrum_streaming_stop();
    receiver_model.disable();
    baseband::shutdown();
}

void SpectrumAnalyzerView::on_hide() {
    waterfall_widget.on_hide();
    baseband::spectrum_streaming_stop();
    View::on_hide();
}

void SpectrumAnalyzerView::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);

    const ui::Rect fft_rect{0, header_height, new_parent_rect.width(), fft_height};
    fft_view.set_parent_rect(fft_rect);

    const ui::Rect scale_rect{0, header_height + fft_height, new_parent_rect.width(), scale_height};
    frequency_scale.set_parent_rect(scale_rect);

    const ui::Rect waterfall_rect{0, header_height + fft_height + scale_height,
                                   new_parent_rect.width(),
                                   new_parent_rect.height() - header_height - fft_height - scale_height};
    waterfall_widget.set_parent_rect(waterfall_rect);
}

void SpectrumAnalyzerView::focus() {
    field_freq_start.focus();
}

void SpectrumAnalyzerView::on_frequency_changed() {
    rf::Frequency start = field_freq_start.value();
    rf::Frequency end = field_freq_end.value();

    // Ensure end > start
    if (end <= start) {
        end = start + bandwidth_hz;
        field_freq_end.set_value(end);
    }

    // Calculate center frequency
    rf::Frequency center = (start + end) / 2;

    // Update receiver frequency
    receiver_model.set_target_frequency(center);
}

void SpectrumAnalyzerView::on_gain_changed() {
    // Gain changes are handled by the field callbacks
    // This can be used for additional processing if needed
}

void SpectrumAnalyzerView::update_receiver() {
    receiver_model.set_modulation(ReceiverModel::Mode::SpectrumAnalysis);
    receiver_model.set_sampling_rate(bandwidth_hz);
    receiver_model.set_baseband_bandwidth(bandwidth_hz);
    receiver_model.enable();
}

void SpectrumAnalyzerView::on_channel_spectrum(const ChannelSpectrum& spectrum) {
    // Pass to FFT view for display
    fft_view.on_channel_spectrum(spectrum);
    
    // Pass to waterfall widget
    waterfall_widget.on_channel_spectrum(spectrum);
    
    // Update frequency scale
    frequency_scale.set_spectrum_sampling_rate(spectrum.sampling_rate);
    frequency_scale.set_channel_filter(
        spectrum.channel_filter_low_frequency,
        spectrum.channel_filter_high_frequency,
        spectrum.channel_filter_transition);
}

}  // namespace ui::external_app::spectrum_analyzer
