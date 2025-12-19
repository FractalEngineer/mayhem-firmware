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
#include "radio.hpp"
using namespace portapack;

#include "string_format.hpp"
#include <algorithm>

namespace ui::external_app::spectrum_analyzer {

/* SpectrumFFTView **********************************************************/

SpectrumFFTView::SpectrumFFTView(const Rect parent_rect)
    : View{parent_rect} {
    set_focusable(false);

    // Update waveform widgets to use the parent rect's height instead of hardcoded 2*16
    waveform.set_parent_rect({0, 0, parent_rect.width(), parent_rect.height()});
    peak_hold_waveform.set_parent_rect({0, 0, parent_rect.width(), parent_rect.height()});

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

void SpectrumFFTView::set_parent_rect(const Rect new_parent_rect) {
    View::set_parent_rect(new_parent_rect);
    
    // Update waveform widgets to match the new parent rect size
    waveform.set_parent_rect({0, 0, new_parent_rect.width(), new_parent_rect.height()});
    peak_hold_waveform.set_parent_rect({0, 0, new_parent_rect.width(), new_parent_rect.height()});
}

void SpectrumFFTView::get_max_power(const ChannelSpectrum& spectrum, uint8_t bin, uint8_t& max_power, bool sweeping) {
    if (!sweeping) {
        // Single pass mode: use standard mapping
        if (bin < 120) {
            if (spectrum.db[256 - 120 + bin] > max_power)
                max_power = spectrum.db[256 - 120 + bin];
        } else {
            if (spectrum.db[bin - 120] > max_power)
                max_power = spectrum.db[bin - 120];
        }
    } else {
        // Sweeping mode: use FASTSCAN mapping (like looking glass)
        if (bin < 120) {
            if (spectrum.db[134 + bin] > max_power)
                max_power = spectrum.db[134 + bin];
        } else {
            if (spectrum.db[bin - 118] > max_power)
                max_power = spectrum.db[bin - 118];
        }
    }
}

bool SpectrumFFTView::process_bins(uint8_t* powerlevel, Gradient& gradient, std::vector<Color>& waterfall_row, bool fft_paused) {
    bins_hz_size_ += each_bin_size_;  // Add Hz coverage from this bin
    if (bins_hz_size_ >= marker_pixel_step_)  // Enough Hz accumulated for a pixel
    {
        // Convert power to waveform value
        int16_t value = ((int16_t)(*powerlevel) - 128) * 256;
        
        // Only update FFT data if not paused
        if (!fft_paused) {
            spectrum_data[pixel_index_] = value;
            
            // Update peak hold if enabled
            if (peak_hold_enabled) {
                if (value > peak_hold_data[pixel_index_]) {
                    peak_hold_data[pixel_index_] = value;
                }
            }
        }
        
        // Always accumulate waterfall row (even when FFT is paused)
        if (pixel_index_ < waterfall_row.size()) {
            waterfall_row[pixel_index_] = gradient.lut[*powerlevel];
        }
        
        *powerlevel = 0;  // Reset for next accumulation
        pixel_index_++;

        if (pixel_index_ >= display_bins)  // Completed a full line
        {
            bins_hz_size_ = 0;  // Reset for next line
            pixel_index_ = 0;
            // Only mark FFT dirty if not paused
            if (!fft_paused) {
                waveform.set_dirty();
                if (peak_hold_enabled) {
                    peak_hold_waveform.set_dirty();
                }
                set_dirty();
            }
            return true;  // Signal that a new line is complete
        }
        bins_hz_size_ -= marker_pixel_step_;  // Carry excess Hz to next pixel
    }
    return false;
}

bool SpectrumFFTView::on_channel_spectrum(const ChannelSpectrum& spectrum,
                                           rf::Frequency center_freq,
                                           rf::Frequency range_start,
                                           rf::Frequency range_end,
                                           bool sweeping,
                                           rf::Frequency marker_pixel_step,
                                           rf::Frequency each_bin_size,
                                           Gradient& gradient,
                                           std::vector<Color>& waterfall_row) {
    // Update accumulation parameters (always needed for waterfall)
    marker_pixel_step_ = marker_pixel_step;
    each_bin_size_ = each_bin_size;
    
    // If FFT is paused, only process waterfall, skip FFT waveform updates
    bool fft_paused = waveform.is_paused();
    
    if (sweeping) {
        // Sweeping mode: accumulate pixels bin by bin (like looking glass)
        constexpr size_t bin_length = 240;  // Process all bins (screen_width)
        constexpr size_t ignore_dc = 4;  // Ignore DC bins (like FASTSCAN)
        
        for (uint8_t bin = 0; bin < bin_length; bin++) {
            get_max_power(spectrum, bin, max_power_, sweeping);
            
            // Process DC spike if at bin 119
            if (bin == 119) {
                uint8_t next_max_power = 0;
                get_max_power(spectrum, bin + 1, next_max_power, sweeping);
                for (uint8_t it = 0; it < ignore_dc; it++) {
                    uint8_t med_max_power = (max_power_ + next_max_power) / 2;
                    if (process_bins(&med_max_power, gradient, waterfall_row, fft_paused)) {
                        return true;  // New line complete, return
                    }
                }
            }
            
            // Process actual bin
            if (process_bins(&max_power_, gradient, waterfall_row, fft_paused)) {
                return true;  // New line complete, return
            }
        }
        return false;  // Line not yet complete
    } else {
        // Single pass mode: fill all pixels directly
        for (size_t i = 0; i < display_bins; i++) {
            size_t bin_idx;
            // Map to match waterfall display: first half uses upper bins, second half uses lower bins
            if (i < display_bins / 2) {
                bin_idx = 256 - display_bins / 2 + i;
            } else {
                bin_idx = i - display_bins / 2;
            }

            // Convert dB value to waveform value
            int16_t value = ((int16_t)spectrum.db[bin_idx] - 128) * 256;
            
            // Only update FFT data if not paused
            if (!fft_paused) {
                spectrum_data[i] = value;

                // Update peak hold if enabled
                if (peak_hold_enabled) {
                    if (value > peak_hold_data[i]) {
                        peak_hold_data[i] = value;
                    }
                }
            }
            
            // Always update waterfall row (even when FFT is paused)
            if (i < waterfall_row.size()) {
                uint8_t powerlevel = spectrum.db[bin_idx];
                waterfall_row[i] = gradient.lut[powerlevel];
            }
        }
        
        // Only mark FFT dirty if not paused
        if (!fft_paused) {
            waveform.set_dirty();
            if (peak_hold_enabled) {
                peak_hold_waveform.set_dirty();
            }
            set_dirty();
        }
        return false;  // No line completion in single pass mode
    }
}

void SpectrumFFTView::set_peak_hold(bool enabled) {
    peak_hold_enabled = enabled;
    if (!enabled) {
        clear_peak_hold();
        // Hide the peak hold waveform when disabled
        peak_hold_waveform.hidden(true);
    } else {
        // Clear peak hold when enabling so it starts at zero gain (bottom)
        clear_peak_hold();
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
    // Initialize to minimum value (db=0, bottom of screen) so peak hold starts at zero gain
    // This corresponds to -32768 in our conversion: ((0 - 128) * 256) = -32768
    std::fill(peak_hold_data, peak_hold_data + display_bins, -32768);
    peak_hold_waveform.set_dirty();
}

/* SpectrumAnalyzerView ******************************************************/

SpectrumAnalyzerView::SpectrumAnalyzerView(NavigationView& nav)
    : nav_(nav) {
    // Start baseband with wideband spectrum processor
    baseband::run_image(portapack::spi_flash::image_tag_wideband_spectrum);
    
    // Initialize waterfall accumulation
    spectrum_row_.resize(screen_width);

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
    
    // Initialize sweep variables
    freq_start_ = field_freq_start.value();
    freq_end_ = field_freq_end.value();
    // Always use absolute range to handle cases where start > end
    freq_range_ = (freq_end_ > freq_start_) ? (freq_end_ - freq_start_) : (freq_start_ - freq_end_);
    sweeping_ = false;
    f_center_ = 0;
    f_center_ini_ = 0;
    f_center_end_ = 0;
    sweep_step_ = 0;
    sweep_direction_ = 1;
    
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
    // Always use correct min/max regardless of which field is higher
    if (freq_start_ > 0 && freq_end_ > 0) {
        rf::Frequency freq_min = std::min(freq_start_, freq_end_);
        rf::Frequency freq_max = std::max(freq_start_, freq_end_);
        marker_freq_ = (freq_min + freq_max) / 2;
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
        // Always use correct min/max regardless of which field is higher
        rf::Frequency freq_min = std::min(freq_start_, freq_end_);
        rf::Frequency freq_max = std::max(freq_start_, freq_end_);
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
        // Always use correct min/max regardless of which field is higher
        rf::Frequency freq_min = std::min(freq_start_, freq_end_);
        rf::Frequency freq_max = std::max(freq_start_, freq_end_);
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

    // Setup receiver for 20MHz bandwidth (must be before on_frequency_changed)
    update_receiver();

    // Initialize sweep parameters based on current frequency range
    // This will also start spectrum streaming
    on_frequency_changed();
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

    // Add 8px spacing between FFT and waterfall for marker triangle
    constexpr ui::Dim fft_waterfall_spacing = 8;
    const ui::Rect waterfall_rect{0, header_height + fft_height + fft_waterfall_spacing,
                                   new_parent_rect.width(),
                                   new_parent_rect.height() - header_height - fft_height - fft_waterfall_spacing};
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
    // Always use absolute range to handle cases where start > end
    freq_range_ = (end > start) ? (end - start) : (start - end);

    // Determine if we need to sweep (range > bandwidth)
    sweeping_ = (freq_range_ > bandwidth_hz);
    
    if (sweeping_) {
        // Calculate sweep parameters (matching looking glass app FASTSCAN mode)
        // Start center frequency: start + bandwidth/2
        f_center_ini_ = start + (bandwidth_hz / 2);
        // End center frequency: end - bandwidth/2
        f_center_end_ = end - (bandwidth_hz / 2);
        // Step size calculation matches looking glass app:
        // step = (bin_length + ignore_dc) * (bandwidth / spec_nb_bins)
        // This gives approximately 19.06 MHz step size with minimal overlap
        rf::Frequency each_bin_size = bandwidth_hz / spec_nb_bins;
        sweep_step_ = (sweep_bin_length + sweep_ignore_dc) * each_bin_size;
        
        // Initialize current center frequency
        f_center_ = f_center_ini_;
        sweep_direction_ = 1;  // Start sweeping forward
    } else {
        // Single pass mode: just use center of range
        f_center_ = (start + end) / 2;
        f_center_ini_ = f_center_;
        f_center_end_ = f_center_;
        sweep_step_ = 0;
    }

    // Restart spectrum streaming when frequency changes
    baseband::spectrum_streaming_stop();
    
    // Update receiver frequency
    if (sweeping_) {
        // Use direct tuning for faster sweeps (like looking glass app)
        radio::set_tuning_frequency(f_center_);
    } else {
        receiver_model.set_target_frequency(f_center_);
    }
    
    // Restart spectrum streaming
    baseband::spectrum_streaming_start();
    
    // Reset marker to center of MIN/MAX range when frequencies change
    // Always use correct min/max regardless of which field is higher
    rf::Frequency freq_min = std::min(start, end);
    rf::Frequency freq_max = std::max(start, end);
    marker_pixel_index_ = screen_width / 2;
    marker_freq_ = (freq_min + freq_max) / 2;
    
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

void SpectrumAnalyzerView::retune() {
    // Change center frequency during sweep
    // Use direct tuning for faster sweeps (like looking glass app)
    radio::set_tuning_frequency(f_center_);
    chThdSleepMilliseconds(5);  // Stabilize frequency
    baseband::spectrum_streaming_start();  // Restart spectrum capture
}

void SpectrumAnalyzerView::update_gain_display() {
    if (latest_spectrum.sampling_rate == 0) {
        field_freq_mark.set_text("---");
        text_gain.set("---");
        return;
    }
    
    // Calculate marker frequency from pixel position using MIN/MAX range
    // Always use correct min/max regardless of which field is higher
    rf::Frequency freq_min = std::min(freq_start_, freq_end_);
    rf::Frequency freq_max = std::max(freq_start_, freq_end_);
    rf::Frequency freq_range = freq_max - freq_min;
    // Map pixel position to frequency: pixel 0 = MIN, pixel screen_width-1 = MAX
    rf::Frequency marker = freq_min + (marker_pixel_index_ * freq_range) / (screen_width - 1);
    
    // Clamp marker frequency to MIN/MAX range (safety check)
    if (marker < freq_min) marker = freq_min;
    if (marker > freq_max) marker = freq_max;
    marker_freq_ = marker;  // Store for reference
    
    // Convert marker frequency to bin index
    size_t bin_index;
    
    if (sweeping_) {
        // When sweeping, calculate bin based on frequency offset from current center
        rf::Frequency freq_offset = marker - f_center_;  // Offset from center frequency
        rf::Frequency half_bandwidth = bandwidth_hz / 2;
        
        // Check if marker frequency is within current bandwidth
        if (freq_offset < -half_bandwidth || freq_offset > half_bandwidth) {
            // Marker is outside current sweep position, show "---" for gain
            // Throttle updates: only update display every 8 cycles
            update_counter_++;
            constexpr uint32_t update_interval = 8;
            bool should_update = (update_counter_ % update_interval == 0) || (update_counter_ == 1);
            
            if (should_update) {
                field_freq_mark.set_text(to_string_short_freq(marker));
                text_gain.set("---");
            }
            plot_marker();
            return;
        }
        
        // Map frequency offset to bin index
        // Spectrum bins: bin 0 = center, bins 1-127 = positive offset, bins 128-255 = negative offset
        // Display mapping: first half (0-119) = bins 136-255 (negative), second half (120-239) = bins 0-119 (positive)
        rf::Frequency bin_hz = latest_spectrum.sampling_rate / 256;  // Hz per bin
        int32_t bin_offset = static_cast<int32_t>(freq_offset / bin_hz);
        
        // Positive offset (marker > center) maps to bins 0-127, but display uses bins 0-119
        // Negative offset (marker < center) maps to bins 128-255, but display uses bins 136-255
        if (bin_offset >= 0) {
            // Positive offset: use bins 0-119 (second half of screen)
            bin_index = static_cast<size_t>(bin_offset);
            if (bin_index >= 120) bin_index = 119;  // Clamp to display range
        } else {
            // Negative offset: use bins 136-255 (first half of screen)
            // Map negative offset to bins 136-255: bin_offset = -1 -> bin 255, bin_offset = -120 -> bin 136
            bin_index = 256 + bin_offset;  // bin_offset is negative, so this gives us the right bin
            if (bin_index < 136) bin_index = 136;  // Clamp to display range
            if (bin_index >= 256) bin_index = 255;
        }
    } else {
        // Single pass mode: use original mapping based on screen position
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
    
    // Calculate accumulation parameters
    rf::Frequency marker_pixel_step = freq_range_ / screen_width;  // Hz per pixel
    constexpr size_t spec_nb_bins = 256;
    rf::Frequency each_bin_size = bandwidth_hz / spec_nb_bins;  // Hz per bin
    
    // Pass to FFT view for display with frequency mapping info
    bool line_complete = fft_view.on_channel_spectrum(spectrum, f_center_, freq_start_, freq_end_, sweeping_, 
                                                       marker_pixel_step, each_bin_size,
                                                       waterfall_widget.gradient, spectrum_row_);
    
    // Handle waterfall display
    if (sweeping_ && line_complete) {
        // When a full line is complete during sweeping, draw the accumulated waterfall row
        const auto waterfall_rect = waterfall_widget.screen_rect();
        const auto draw_y = portapack::display.scroll(1);
        portapack::display.draw_pixels({{waterfall_rect.left(), draw_y}, {screen_width, 1}}, spectrum_row_);
    } else if (!sweeping_) {
        // Single pass mode: use standard waterfall
        waterfall_widget.on_channel_spectrum(spectrum);
    }
    
    // Update gain display
    update_gain_display();
    
    // Redraw marker (in case FFT/waterfall redraw overwrote it)
    plot_marker();
    
    // Handle frequency sweeping if range > bandwidth
    if (sweeping_) {
        baseband::spectrum_streaming_stop();
        
        // Update center frequency for next sweep step
        f_center_ += sweep_direction_ * sweep_step_;
        
        // Check if we've reached the end of the sweep range
        if (sweep_direction_ > 0) {
            // Sweeping forward
            if (f_center_ >= f_center_end_) {
                // Reached end, reverse direction
                f_center_ = f_center_end_;
                sweep_direction_ = -1;
            }
        } else {
            // Sweeping backward
            if (f_center_ <= f_center_ini_) {
                // Reached start, reverse direction
                f_center_ = f_center_ini_;
                sweep_direction_ = 1;
            }
        }
        
        // Retune to new center frequency
        retune();
    } else {
        // Single pass mode: just restart streaming
        baseband::spectrum_streaming_start();
    }
}

void SpectrumAnalyzerView::plot_marker() {
    // Draw marker triangle in the 8px spacing between FFT and waterfall
    const ui::Rect fft_rect = fft_view.screen_rect();
    constexpr ui::Dim fft_waterfall_spacing = 8;
    const Coord marker_y = fft_rect.bottom();  // Start at bottom of FFT, in the spacing area
    
    // Clamp marker position to valid range (uint8_t can't be < 0, so only check upper bound)
    uint8_t pos = marker_pixel_index_;
    if (pos >= screen_width) pos = screen_width - 1;
    
    // Clear old marker area (8 pixels tall to cover the triangle and spacing)
    portapack::display.fill_rectangle(
        {0, marker_y, screen_width, fft_waterfall_spacing},
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
