import 'package:flutter/services.dart';

import '../models/display_info.dart';

/// Service that communicates with platform-native brightness APIs
/// via a [MethodChannel].
final class BrightnessService {
  BrightnessService._();

  static final BrightnessService instance = BrightnessService._();

  static const _channel = MethodChannel('com.bsdisplaycontrol/brightness');

  /// Retrieves all connected displays with their current brightness levels.
  Future<List<DisplayInfo>> getDisplays() async {
    final result = await _channel.invokeMethod<List<dynamic>>('getDisplays');
    if (result == null) return [];
    return result
        .cast<Map<dynamic, dynamic>>()
        .map((m) => DisplayInfo.fromMap(Map<String, dynamic>.from(m)))
        .toList();
  }

  /// Sets the brightness for a specific display.
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
}
