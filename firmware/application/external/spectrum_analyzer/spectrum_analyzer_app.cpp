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

    // Peak hold starts disabled, so hide the waveform initially
    peak_hold_waveform.hidden(true);
    
    // Set up callback to sync peak hold pause state with main waveform
    waveform.on_select = [this](Waveform&) {
        // When main waveform is paused/unpaused, sync peak hold waveform
        peak_hold_waveform.set_paused(waveform.is_paused());
        // Also hide peak hold when paused (even if peak hold is enabled)
        if (waveform.is_paused()) {
            peak_hold_waveform.hidden(true);
        } else if (peak_hold_enabled) {
            peak_hold_waveform.hidden(false);
        }
    };

    add_children({&waveform, &peak_hold_waveform});
}

void SpectrumFFTView::paint(Painter& painter) {
    const auto r = screen_rect();
    painter.fill_rectangle(r, Theme::getInstance()->bg_darkest->background);
}

void SpectrumFFTView::on_channel_spectrum(const ChannelSpectrum& spectrum) {
    // Don't update if waveform is paused (user clicked to turn off FFT)
    if (waveform.is_paused()) {
        return;
    }
    
    // Convert spectrum to display format
    // Spectrum has 256 bins, we need 240 for display
    // Map spectrum bins similar to how waterfall does it
    for (size_t i = 0; i < display_bins; i++) {
        size_t bin_idx;
        // Map to match waterfall display: first half uses upper bins, second half uses lower bins
        if (i < display_bins / 2) {
            bin_idx = 256 - display_bins / 2 + i;
        } else {
            bin_idx = i - display_bins / 2;
        }

        // Convert dB value to waveform value with zero at bottom
        // spectrum.db is 0-255, convert to -32768 to 32767 range for waveform
        // This maps minimum dB (0) to bottom of screen and maximum dB (255) to top
        // Use negative values for low dB (bottom) and positive for high dB (top)
        // Center at 128 so db=0 maps to -32768 (bottom) and db=255 maps to 32512 (top)
        int16_t value = ((int16_t)spectrum.db[bin_idx] - 128) * 256;
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
    
    // Request marker redraw (will be handled by parent view)
    set_dirty();
}

void SpectrumFFTView::set_peak_hold(bool enabled) {
    peak_hold_enabled = enabled;
    if (!enabled) {
        clear_peak_hold();
        // Hide the peak hold waveform when disabled
        peak_hold_waveform.hidden(true);
    } else {
        // Show the peak hold waveform when enabled, but only if FFT is not paused
        if (waveform.is_paused()) {
            peak_hold_waveform.hidden(true);
            peak_hold_waveform.set_paused(true);
        } else {
            peak_hold_waveform.hidden(false);
            peak_hold_waveform.set_paused(false);
        }
    }
    set_dirty();  // Request redraw to show/hide peak hold
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
                  &labels,
                  &field_freq_start,
                  &field_freq_end,
                  &field_freq_mark,
                  &text_gain,
                  &field_lna,
                  &field_vga,
                  &field_rf_amp,
                  &checkbox_peak_hold,
                  &fft_view,
                  &waterfall_widget});

    // Initialize frequency fields from persistent settings or defaults
    rf::Frequency center_freq = receiver_model.target_frequency();
    if (freq_start_ > 0 && freq_end_ > 0) {
        // Load from persistent settings
        field_freq_start.set_value(freq_start_);
        field_freq_end.set_value(freq_end_);
    } else {
        // Use defaults based on current center frequency
        field_freq_start.set_value(center_freq - bandwidth_hz / 2);
        field_freq_end.set_value(center_freq + bandwidth_hz / 2);
    }
    
    field_freq_start.updated = [this](rf::Frequency) {
        freq_start_ = field_freq_start.value();
        this->on_frequency_changed();
    };

    field_freq_end.updated = [this](rf::Frequency) {
        freq_end_ = field_freq_end.value();
        this->on_frequency_changed();
    };
    
    // Initialize marker pixel position to center
    marker_pixel_index_ = screen_width / 2;
    
    // Initialize marker frequency to center of MIN/MAX range
    if (freq_start_ > 0 && freq_end_ > 0) {
        marker_freq_ = (freq_start_ + freq_end_) / 2;
    } else {
        marker_freq_ = center_freq;
    }
    
    // Initialize marker and gain display
    field_freq_mark.set_text("---");
    text_gain.set("---");
    
    // Setup marker encoder scrolling (clamp to MIN/MAX range, no wrapping)
    field_freq_mark.on_encoder_change = [this](TextField&, EncoderEvent delta) {
        int32_t new_index = marker_pixel_index_ + delta;
        // Clamp to valid screen range (0 to screen_width-1)
        if (new_index < 0)
            marker_pixel_index_ = 0;
        else if (new_index >= screen_width)
            marker_pixel_index_ = static_cast<uint8_t>(screen_width - 1);
        else
            marker_pixel_index_ = static_cast<uint8_t>(new_index);
        
        // Update marker frequency from pixel position using MIN/MAX range
        rf::Frequency freq_min = freq_start_;
        rf::Frequency freq_max = freq_end_;
        rf::Frequency freq_range = freq_max - freq_min;
        // Map pixel position to frequency: pixel 0 = MIN, pixel screen_width-1 = MAX
        marker_freq_ = freq_min + (marker_pixel_index_ * freq_range) / (screen_width - 1);
        
        // Clamp marker frequency to MIN/MAX range (safety check)
        if (marker_freq_ < freq_min) marker_freq_ = freq_min;
        if (marker_freq_ > freq_max) marker_freq_ = freq_max;
        
        // Force immediate update when user moves marker (reset counter)
        update_counter_ = 0;
        update_gain_display();
        plot_marker();  // Draw marker immediately
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

    checkbox_peak_hold.on_select = [this](Checkbox&, bool v) {
        peak_hold_ = v;
        fft_view.set_peak_hold(v);
    };
    
    // Load peak hold setting (must be after on_select is set)
    checkbox_peak_hold.set_value(peak_hold_);
    fft_view.set_peak_hold(peak_hold_);

    waterfall_widget.on_touch_select = [this](int32_t x, int32_t y) {
        if (y > screen_height - screen_height * 0.1) return;
        // Update marker pixel position (clamp to valid range)
        if (x < 0) x = 0;
        if (x >= screen_width) x = screen_width - 1;
        marker_pixel_index_ = static_cast<uint8_t>(x);
        
        // Calculate frequency from touch position using MIN/MAX range
        rf::Frequency freq_min = freq_start_;
        rf::Frequency freq_max = freq_end_;
        rf::Frequency freq_range = freq_max - freq_min;
        // Map pixel position to frequency: pixel 0 = MIN, pixel screen_width-1 = MAX
        marker_freq_ = freq_min + (marker_pixel_index_ * freq_range) / (screen_width - 1);
        
        // Clamp marker frequency to MIN/MAX range (safety check)
        if (marker_freq_ < freq_min) marker_freq_ = freq_min;
        if (marker_freq_ > freq_max) marker_freq_ = freq_max;
        
        // Force immediate update when user touches (reset counter)
        update_counter_ = 0;
        update_gain_display();
        plot_marker();  // Draw marker immediately
    };

    // Load gradient for waterfall (use default if file loading fails)
    waterfall_widget.gradient.set_default();

    // Setup receiver for 20MHz bandwidth
    update_receiver();

    // Start spectrum streaming (must be after receiver is enabled)
    baseband::spectrum_streaming_start();
}

SpectrumAnalyzerView::~SpectrumAnalyzerView() {
    baseband::spectrum_streaming_stop();
    receiver_model.disable();
    baseband::shutdown();
}

void SpectrumAnalyzerView::on_show() {
    // Initialize waterfall widget when view is shown
    waterfall_widget.on_show();
    View::on_show();
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

    const ui::Rect waterfall_rect{0, header_height + fft_height,
                                   new_parent_rect.width(),
                                   new_parent_rect.height() - header_height - fft_height};
    waterfall_widget.set_parent_rect(waterfall_rect);
    
    // Re-initialize waterfall scroll area when rect changes (if already shown)
    if (visible()) {
        waterfall_widget.on_show();
    }
    
    // Redraw marker when rect changes
    plot_marker();
}

void SpectrumAnalyzerView::paint(Painter& painter) {
    // Call base paint to fill background
    View::paint(painter);
    
    // Draw marker on top of everything (after children are painted)
    plot_marker();
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
    
    // Update stored values
    freq_start_ = start;
    freq_end_ = end;

    // Calculate center frequency
    rf::Frequency center = (start + end) / 2;

    // Restart spectrum streaming when frequency changes
    baseband::spectrum_streaming_stop();
    
    // Update receiver frequency
    receiver_model.set_target_frequency(center);
    
    // Restart spectrum streaming
    baseband::spectrum_streaming_start();
    
    // Reset marker to center of MIN/MAX range when frequencies change
    marker_pixel_index_ = screen_width / 2;
    marker_freq_ = center;
    
    // Update marker display
    update_gain_display();
}

void SpectrumAnalyzerView::on_marker_changed() {
    // Marker is updated from touch or spectrum data, not from user input
    // This function is kept for compatibility but marker updates happen in update_gain_display
    update_gain_display();
}

void SpectrumAnalyzerView::on_gain_changed() {
    // Gain changes are handled by the field callbacks
    // This can be used for additional processing if needed
}

void SpectrumAnalyzerView::update_gain_display() {
    if (latest_spectrum.sampling_rate == 0) {
        field_freq_mark.set_text("---");
        text_gain.set("---");
        return;
    }
    
    // Calculate marker frequency from pixel position using MIN/MAX range
    rf::Frequency freq_min = freq_start_;
    rf::Frequency freq_max = freq_end_;
    rf::Frequency freq_range = freq_max - freq_min;
    // Map pixel position to frequency: pixel 0 = MIN, pixel screen_width-1 = MAX
    rf::Frequency marker = freq_min + (marker_pixel_index_ * freq_range) / (screen_width - 1);
    
    // Clamp marker frequency to MIN/MAX range (safety check)
    if (marker < freq_min) marker = freq_min;
    if (marker > freq_max) marker = freq_max;
    marker_freq_ = marker;  // Store for reference
    
    // Convert marker pixel position to bin index (using same mapping as FFT view)
    // The spectrum bins are mapped similar to FFT view and waterfall:
    // First half of screen (0-119): uses bins 256-120+0 to 256-120+119 = bins 136 to 255
    // Second half of screen (120-239): uses bins 0-119
    // Center of screen (120) corresponds to bin 0 (center frequency)
    size_t bin_index;
    int32_t screen_pos = marker_pixel_index_;
    
    if (screen_pos < screen_width / 2) {
        // Lower half: map to bins 136-255
        if (screen_pos < 0) screen_pos = 0;
        bin_index = 256 - (screen_width / 2) + screen_pos;
        if (bin_index >= 256) bin_index = 255;
    } else {
        // Upper half: map to bins 0-119
        bin_index = screen_pos - (screen_width / 2);
        if (bin_index >= 256) bin_index = 255;
    }
    
    // Throttle updates: only update display every 8 cycles (for readability)
    // Always update on first call or when counter wraps
    update_counter_++;
    constexpr uint32_t update_interval = 8;
    bool should_update = (update_counter_ % update_interval == 0) || (update_counter_ == 1);
    
    if (should_update) {
        // Update marker frequency display using short format (like looking glass app)
        field_freq_mark.set_text(to_string_short_freq(marker));
        
        // Get dB value from spectrum
        // spectrum.db is 0-255, converted from dB using: db = (db_value - 255.0f) / 5.0f
        // So to get dB back: db = (spectrum.db[i] - 255.0f) / 5.0f
        uint8_t db_value = latest_spectrum.db[bin_index];
        float db = (static_cast<float>(db_value) - 255.0f) / 5.0f;
        
        // Format as dB value
        text_gain.set(to_string_dec_int(static_cast<int32_t>(db)) + " dB");
    }
    
    // Always draw marker triangle on waterfall (no throttling for visual feedback)
    plot_marker();
}

void SpectrumAnalyzerView::update_receiver() {
    receiver_model.set_modulation(ReceiverModel::Mode::SpectrumAnalysis);
    receiver_model.set_sampling_rate(bandwidth_hz);
    receiver_model.set_baseband_bandwidth(bandwidth_hz);
    receiver_model.enable();
    
    // Configure baseband spectrum processing (must be after receiver is enabled)
    // trigger = 127 means process every 128th buffer (for 20MHz, this gives good update rate)
    constexpr size_t trigger = 127;
    baseband::set_spectrum(bandwidth_hz, trigger);
}

void SpectrumAnalyzerView::on_channel_spectrum(const ChannelSpectrum& spectrum) {
    // Store latest spectrum for gain calculation
    latest_spectrum = spectrum;
    
    // Pass to FFT view for display
    fft_view.on_channel_spectrum(spectrum);
    
    // Pass to waterfall widget (this will draw the waterfall)
    waterfall_widget.on_channel_spectrum(spectrum);
    
    // Update gain display
    update_gain_display();
    
    // Redraw marker (in case FFT/waterfall redraw overwrote it)
    plot_marker();
}

void SpectrumAnalyzerView::plot_marker() {
    // Draw marker triangle on FFT view area (similar to looking glass app)
    // The marker is drawn at the bottom of the FFT view, just above the waterfall
    const ui::Rect fft_rect = fft_view.screen_rect();
    const Coord marker_y = fft_rect.bottom() - 8;  // 8 pixels from bottom of FFT view
    
    // Clamp marker position to valid range (uint8_t can't be < 0, so only check upper bound)
    uint8_t pos = marker_pixel_index_;
    if (pos >= screen_width) pos = screen_width - 1;
    
    // Clear old marker area (8 pixels tall to cover the triangle)
    portapack::display.fill_rectangle(
        {0, marker_y, screen_width, 8},
        Theme::getInstance()->bg_darkest->background);
    
    // Draw triangle marker (pointing down) - same style as looking glass app
    // Top rectangle
    portapack::display.fill_rectangle(
        {static_cast<Coord>(pos - 2), marker_y, 5, 3},
        Theme::getInstance()->fg_red->foreground);
    
    // Middle rectangle
    portapack::display.fill_rectangle(
        {static_cast<Coord>(pos - 1), marker_y + 3, 3, 3},
        Theme::getInstance()->fg_red->foreground);
    
    // Bottom rectangle (point)
    portapack::display.fill_rectangle(
        {static_cast<Coord>(pos), marker_y + 6, 1, 2},
        Theme::getInstance()->fg_red->foreground);
}

}  // namespace ui::external_app::spectrum_analyzer
