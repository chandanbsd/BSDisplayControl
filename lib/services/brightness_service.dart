import 'package:flutter/services.dart';

import '../models/display_info.dart';

/// Service that communicates with platform-native brightness APIs
/// via a [MethodChannel].
final class BrightnessService {
  BrightnessService._();

  static final BrightnessService instance = BrightnessService._();

  static const _channel = MethodChannel(
    'com.chandanbsd.bsdisplaycontrol/brightness',
  );

  /// Retrieves all connected displays with their current brightness levels.
  Future<List<DisplayInfo>> getDisplays() async {
    final result = await _channel.invokeMethod<List<dynamic>>('getDisplays');
    if (result == null) return [];
    return result
        .cast<Map<dynamic, dynamic>>()
        .map((m) => DisplayInfo.fromMap(Map<String, dynamic>.from(m)))
        .toList();
  }

  /// Sets the hardware brightness for a specific display.
  ///
  /// [displayId] is the platform-specific display identifier.
  /// [brightness] is clamped to 0.0-1.0.
  Future<bool> setBrightness({
    required String displayId,
    required double brightness,
  }) async {
    final clamped = brightness.clamp(0.0, 1.0);
    final result = await _channel.invokeMethod<bool>('setBrightness', {
      'displayId': displayId,
      'brightness': clamped,
    });
    return result ?? false;
  }

  /// Sets the software brightness (gamma) for a specific display.
  ///
  /// This applies a gamma ramp adjustment to dim the display below
  /// the hardware minimum. [gamma] is clamped to 0.0-1.0 where:
  /// - 1.0 = normal (no software dimming)
  /// - 0.0 = fully dimmed (black screen)
  ///
  /// Uses platform-specific gamma APIs:
  /// - Linux: xrandr --brightness
  /// - Windows: SetDeviceGammaRamp
  /// - macOS: CGSetDisplayTransferByFormula
  Future<bool> setSoftwareBrightness({
    required String displayId,
    required double gamma,
  }) async {
    final clamped = gamma.clamp(0.0, 1.0);
    final result = await _channel.invokeMethod<bool>('setSoftwareBrightness', {
      'displayId': displayId,
      'gamma': clamped,
    });
    return result ?? false;
  }
}
