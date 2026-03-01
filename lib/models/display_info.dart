/// Represents a physical display connected to the system.
final class DisplayInfo {
  const DisplayInfo({
    required this.id,
    required this.name,
    required this.brightness,
    this.isBuiltIn = false,
    this.softwareBrightness = 1.0,
  });

  /// Platform-specific display identifier.
  final String id;

  /// Human-readable display name.
  final String name;

  /// Hardware brightness level, normalized to 0.0–1.0.
  final double brightness;

  /// Whether this is the built-in display (e.g., laptop panel).
  final bool isBuiltIn;

  /// Software brightness (gamma) level, 0.0–1.0.
  ///
  /// 1.0 = normal (no dimming), 0.0 = fully black.
  /// This is applied as a gamma ramp/overlay on top of hardware brightness,
  /// allowing dimming below the hardware minimum.
  final double softwareBrightness;

  /// The effective brightness combining hardware and software dimming.
  ///
  /// Maps the unified slider range (-0.5 to 1.0) where:
  /// - 0.0 to 1.0 → hardware brightness (software = 1.0)
  /// - -0.5 to 0.0 → hardware at 0, software dims from 1.0 to 0.0
  double get effectiveBrightness {
    if (brightness <= 0.0 && softwareBrightness < 1.0) {
      // In software dimming range: map gamma 1.0→0.0 to slider 0.0→-0.5
      return -(1.0 - softwareBrightness) * 0.5;
    }
    return brightness;
  }

  DisplayInfo copyWith({
    String? id,
    String? name,
    double? brightness,
    bool? isBuiltIn,
    double? softwareBrightness,
  }) {
    return DisplayInfo(
      id: id ?? this.id,
      name: name ?? this.name,
      brightness: brightness ?? this.brightness,
      isBuiltIn: isBuiltIn ?? this.isBuiltIn,
      softwareBrightness: softwareBrightness ?? this.softwareBrightness,
    );
  }

  factory DisplayInfo.fromMap(Map<String, dynamic> map) {
    final id = map['id'];
    final name = map['name'];
    final brightness = map['brightness'];

    if (id is! String) {
      throw FormatException(
        'DisplayInfo.fromMap: "id" must be a String, got ${id.runtimeType}',
      );
    }
    if (name is! String) {
      throw FormatException(
        'DisplayInfo.fromMap: "name" must be a String, got ${name.runtimeType}',
      );
    }
    if (brightness is! num) {
      throw FormatException(
        'DisplayInfo.fromMap: "brightness" must be a num, got ${brightness.runtimeType}',
      );
    }

    final softwareBrightness = map['softwareBrightness'];

    return DisplayInfo(
      id: id,
      name: name,
      brightness: brightness.toDouble(),
      isBuiltIn: map['isBuiltIn'] as bool? ?? false,
      softwareBrightness: (softwareBrightness is num)
          ? softwareBrightness.toDouble()
          : 1.0,
    );
  }

  Map<String, dynamic> toMap() {
    return {
      'id': id,
      'name': name,
      'brightness': brightness,
      'isBuiltIn': isBuiltIn,
      'softwareBrightness': softwareBrightness,
    };
  }

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is DisplayInfo &&
          runtimeType == other.runtimeType &&
          id == other.id;

  @override
  int get hashCode => id.hashCode;

  @override
  String toString() =>
      'DisplayInfo(id: $id, name: $name, brightness: ${(brightness * 100).round()}%, '
      'softwareBrightness: ${(softwareBrightness * 100).round()}%, builtIn: $isBuiltIn)';
}
