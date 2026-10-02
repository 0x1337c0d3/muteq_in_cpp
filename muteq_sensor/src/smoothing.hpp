//
//  smoothing.hpp
//  muteq_sensor
//
//  Asymmetric Exponential Moving Average (EMA) filter for sound pressure levels.
//  Fast attack, slow decay: loud spikes are tracked instantly, while decay is smoothed.
//

#pragma once

#include <algorithm>
#include <optional>

namespace muteq {

/// Exponential moving average with separate rise and fall coefficients.
/// Replicates the AsymmetricEMA filter from mute/sensor/app/smoothing.py.
class AsymmetricEMA {
  public:
    constexpr explicit AsymmetricEMA(float rise_alpha = 0.5f, float fall_alpha = 0.05f) noexcept
        : rise_alpha_(rise_alpha), fall_alpha_(fall_alpha) {
    }

    /// Update the filter with a new sample and return the current smoothed value.
    constexpr float update(float sample) noexcept {
        if (!value_.has_value()) {
            value_ = sample;
            return sample;
        }

        const float alpha = (sample > *value_) ? rise_alpha_ : fall_alpha_;
        *value_ = alpha * sample + (1.0f - alpha) * (*value_);
        return *value_;
    }

    /// Return the current filtered value, or nullopt if no sample has been processed.
    [[nodiscard]] constexpr std::optional<float> value() const noexcept {
        return value_;
    }

    /// Reset filter state.
    constexpr void reset() noexcept {
        value_.reset();
    }

    [[nodiscard]] constexpr float rise_alpha() const noexcept {
        return rise_alpha_;
    }
    [[nodiscard]] constexpr float fall_alpha() const noexcept {
        return fall_alpha_;
    }

    constexpr void set_coefficients(float rise_alpha, float fall_alpha) noexcept {
        rise_alpha_ = rise_alpha;
        fall_alpha_ = fall_alpha;
    }

  private:
    float rise_alpha_{0.5f};
    float fall_alpha_{0.05f};
    std::optional<float> value_{};
};

} // namespace muteq
