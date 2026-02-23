import 'package:flutter/material.dart';

import '../models/display_info.dart';

/// Brightness presets with name and value.
const _presets = <(String, IconData, double)>[
  ('Dim', Icons.brightness_low, 0.25),
  ('Half', Icons.brightness_medium, 0.50),
  ('Bright', Icons.brightness_high, 0.75),
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
  final ValueChanged<double> onBrightnessChanged;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final brightnessPercent = (display.brightness * 100).round();

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
                    color: theme.colorScheme.primaryContainer,
                    borderRadius: BorderRadius.circular(20),
                  ),
                  child: Text(
                    '$brightnessPercent%',
                    style: theme.textTheme.titleSmall?.copyWith(
                      color: theme.colorScheme.onPrimaryContainer,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 20),

            // Brightness slider
            Row(
              children: [
                Icon(
                  Icons.brightness_low,
                  size: 20,
                  color: theme.colorScheme.onSurfaceVariant,
                ),
                Expanded(
                  child: Slider(
                    value: display.brightness,
                    onChanged: onBrightnessChanged,
                    divisions: 100,
                    label: '$brightnessPercent%',
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
                final isActive = (display.brightness - value).abs() < 0.01;
                return FilledButton.tonalIcon(
                  onPressed: () => onBrightnessChanged(value),
                  icon: Icon(icon, size: 18),
                  label: Text(label),
                  style: FilledButton.styleFrom(
                    backgroundColor: isActive
                        ? theme.colorScheme.primary
                        : null,
                    foregroundColor: isActive
                        ? theme.colorScheme.onPrimary
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
