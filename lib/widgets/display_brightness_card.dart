import 'package:flutter/material.dart';

import '../models/display_info.dart';

/// Minimum slider value (-50% = maximum software dimming).
const double kMinSliderValue = -0.5;

/// Maximum slider value (100% = maximum hardware brightness).
const double kMaxSliderValue = 1.0;

/// Total number of slider divisions (150 = 1% per step from -50% to 100%).
const int kSliderDivisions = 150;

/// Brightness presets with name, icon, and value.
const _presets = <(String, IconData, double)>[
  ('Night', Icons.nightlight_round, -0.25),
  ('Dim', Icons.brightness_low, 0.25),
  ('Half', Icons.brightness_medium, 0.50),
  ('Max', Icons.brightness_7, 1.00),
];

/// Card widget that shows a display's name, brightness slider, and presets.
class DisplayBrightnessCard extends StatelessWidget {
  const DisplayBrightnessCard({
    super.key,
    required this.display,
    required this.onBrightnessChanged,
  });

  final DisplayInfo display;

  /// Called with the unified slider value (-0.5 to 1.0).
  final ValueChanged<double> onBrightnessChanged;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final sliderValue = display.effectiveBrightness;
    final displayPercent = (sliderValue * 100).round();
    final isSoftwareDimming = sliderValue < 0;

    return Card(
      elevation: 2,
      margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            // Display header
            Row(
              children: [
                Icon(
                  display.isBuiltIn ? Icons.laptop : Icons.monitor,
                  size: 28,
                  color: theme.colorScheme.primary,
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        display.name,
                        style: theme.textTheme.titleMedium?.copyWith(
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                      if (display.isBuiltIn)
                        Text(
                          'Built-in Display',
                          style: theme.textTheme.bodySmall?.copyWith(
                            color: theme.colorScheme.onSurfaceVariant,
                          ),
                        ),
                    ],
                  ),
                ),
                // Brightness percentage badge
                Container(
                  padding: const EdgeInsets.symmetric(
                    horizontal: 12,
                    vertical: 6,
                  ),
                  decoration: BoxDecoration(
                    color: isSoftwareDimming
                        ? theme.colorScheme.tertiaryContainer
                        : theme.colorScheme.primaryContainer,
                    borderRadius: BorderRadius.circular(20),
                  ),
                  child: Text(
                    '$displayPercent%',
                    style: theme.textTheme.titleSmall?.copyWith(
                      color: isSoftwareDimming
                          ? theme.colorScheme.onTertiaryContainer
                          : theme.colorScheme.onPrimaryContainer,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 20),

            // Software dimming indicator
            if (isSoftwareDimming)
              Padding(
                padding: const EdgeInsets.only(bottom: 8),
                child: Row(
                  children: [
                    Icon(
                      Icons.nightlight_round,
                      size: 14,
                      color: theme.colorScheme.tertiary,
                    ),
                    const SizedBox(width: 6),
                    Text(
                      'Software dimming active',
                      style: theme.textTheme.labelSmall?.copyWith(
                        color: theme.colorScheme.tertiary,
                      ),
                    ),
                  ],
                ),
              ),

            // Brightness slider
            Row(
              children: [
                Icon(
                  Icons.brightness_low,
                  size: 20,
                  color: theme.colorScheme.onSurfaceVariant,
                ),
                Expanded(
                  child: SliderTheme(
                    data: SliderTheme.of(context).copyWith(
                      // Show a different track color in the software dimming zone.
                      activeTrackColor: isSoftwareDimming
                          ? theme.colorScheme.tertiary
                          : null,
                      thumbColor: isSoftwareDimming
                          ? theme.colorScheme.tertiary
                          : null,
                    ),
                    child: Slider(
                      value: sliderValue,
                      min: kMinSliderValue,
                      max: kMaxSliderValue,
                      divisions: kSliderDivisions,
                      label: '$displayPercent%',
                      onChanged: onBrightnessChanged,
                    ),
                  ),
                ),
                Icon(
                  Icons.brightness_high,
                  size: 20,
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ],
            ),
            const SizedBox(height: 12),

            // Preset buttons
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
              children: _presets.map((preset) {
                final (label, icon, value) = preset;
                final isActive = (sliderValue - value).abs() < 0.01;
                final isNight = value < 0;
                return FilledButton.tonalIcon(
                  onPressed: () => onBrightnessChanged(value),
                  icon: Icon(icon, size: 18),
                  label: Text(label),
                  style: FilledButton.styleFrom(
                    backgroundColor: isActive
                        ? (isNight
                              ? theme.colorScheme.tertiary
                              : theme.colorScheme.primary)
                        : null,
                    foregroundColor: isActive
                        ? (isNight
                              ? theme.colorScheme.onTertiary
                              : theme.colorScheme.onPrimary)
                        : null,
                  ),
                );
              }).toList(),
            ),
          ],
        ),
      ),
    );
  }
}
