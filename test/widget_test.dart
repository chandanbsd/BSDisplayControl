import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:bs_display_control/main.dart';

void main() {
  testWidgets('App renders without crashing', (WidgetTester tester) async {
    await tester.pumpWidget(const BSDisplayControlApp());

    // Verify the app title is shown.
    expect(find.text('Display Control'), findsOneWidget);

    // Verify the refresh button exists.
    expect(find.byIcon(Icons.refresh), findsOneWidget);
  });
}
