/// Represents a physical display connected to the system.
final class DisplayInfo {
  const DisplayInfo({
    required this.id,
    required this.name,
    required this.brightness,
    this.isBuiltIn = false,
  });

  /// Platform-specific display identifier.
  final String id;

  /// Human-readable display name.
  final String name;

  /// Current brightness level, normalized to 0.0–1.0.
  final double brightness;

  /// Whether this is the built-in display (e.g., laptop panel).
  final bool isBuiltIn;

  DisplayInfo copyWith({
    String? id,
    String? name,
    double? brightness,
    bool? isBuiltIn,
  }) {
    return DisplayInfo(
      id: id ?? this.id,
      name: name ?? this.name,
      brightness: brightness ?? this.brightness,
      isBuiltIn: isBuiltIn ?? this.isBuiltIn,
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

    return DisplayInfo(
      id: id,
      name: name,
      brightness: brightness.toDouble(),
      isBuiltIn: map['isBuiltIn'] as bool? ?? false,
    );
  }

  Map<String, dynamic> toMap() {
    return {
      'id': id,
      'name': name,
      'brightness': brightness,
      'isBuiltIn': isBuiltIn,
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
      'DisplayInfo(id: $id, name: $name, brightness: ${(brightness * 100).round()}%, builtIn: $isBuiltIn)';
}
