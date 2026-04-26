import 'dart:async';

import 'package:flutter/material.dart';

import '../models/display_info.dart';
import '../services/brightness_service.dart';
import '../widgets/display_brightness_card.dart';

/// The main home screen showing all connected displays.
class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _brightnessService = BrightnessService.instance;

  List<DisplayInfo> _displays = [];
  bool _isLoading = true;
  String? _error;
  Timer? _debounceTimer;

  @override
  void initState() {
    super.initState();
    _loadDisplays();
  }

  @override
  void dispose() {
    _debounceTimer?.cancel();
    super.dispose();
  }

  Future<void> _loadDisplays() async {
    setState(() {
      _isLoading = true;
      _error = null;
    });

    try {
      final displays = await _brightnessService.getDisplays();
      if (!mounted) return;
      setState(() {
        _displays = displays;
        _isLoading = false;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _error = e.toString();
        _isLoading = false;
      });
    }
  }

  void _onBrightnessChanged(DisplayInfo display, double value) {
    // Update UI immediately for responsiveness.
    setState(() {
      _displays = _displays.map((d) {
        if (d.id == display.id) {
          return d.copyWith(brightness: value);
        }
        return d;
      }).toList();
    });

    // Debounce the actual platform call to avoid flooding.
    _debounceTimer?.cancel();
    _debounceTimer = Timer(const Duration(milliseconds: 16), () async {
      try {
        await _brightnessService.setBrightness(
          displayId: display.id,
          brightness: value,
        );
      } catch (e) {
        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('Failed to set brightness: $e'),
            behavior: SnackBarBehavior.floating,
            duration: const Duration(seconds: 2),
          ),
        );
        // Reload to get the actual brightness from the platform.
        _loadDisplays();
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Scaffold(
      appBar: AppBar(
        title: const Text('Display Control'),
        centerTitle: true,
        actions: [
          IconButton(
            onPressed: _loadDisplays,
            icon: const Icon(Icons.refresh),
            tooltip: 'Refresh displays',
          ),
        ],
      ),
      body: switch ((_isLoading, _error)) {
        (true, _) => const Center(child: CircularProgressIndicator()),
        (_, final String error) => _ErrorView(
          message: error,
          onRetry: _loadDisplays,
        ),
        _ when _displays.isEmpty => _EmptyView(onRetry: _loadDisplays),
        _ => ListView.builder(
          padding: const EdgeInsets.symmetric(vertical: 16),
          itemCount: _displays.length + 1,
          itemBuilder: (context, index) {
            if (index == 0) {
              return Padding(
                padding: const EdgeInsets.symmetric(
                  horizontal: 24,
                  vertical: 8,
                ),
                child: Text(
                  '${_displays.length} display${_displays.length == 1 ? '' : 's'} detected',
                  style: theme.textTheme.bodyMedium?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                  ),
                ),
              );
            }
            final display = _displays[index - 1];
            return DisplayBrightnessCard(
              display: display,
              onBrightnessChanged: (value) =>
                  _onBrightnessChanged(display, value),
            );
          },
        ),
      },
    );
  }
}

class _ErrorView extends StatelessWidget {
  const _ErrorView({required this.message, required this.onRetry});

  final String message;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(Icons.error_outline, size: 64, color: theme.colorScheme.error),
            const SizedBox(height: 16),
            Text('Failed to load displays', style: theme.textTheme.titleLarge),
            const SizedBox(height: 8),
            Text(
              message,
              style: theme.textTheme.bodyMedium?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
              ),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 24),
            FilledButton.icon(
              onPressed: onRetry,
              icon: const Icon(Icons.refresh),
              label: const Text('Retry'),
            ),
          ],
        ),
      ),
    );
  }
}

class _EmptyView extends StatelessWidget {
  const _EmptyView({required this.onRetry});

  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              Icons.monitor_outlined,
              size: 64,
              color: theme.colorScheme.onSurfaceVariant,
            ),
            const SizedBox(height: 16),
            Text('No displays found', style: theme.textTheme.titleLarge),
            const SizedBox(height: 8),
            Text(
              'Could not detect any displays with adjustable brightness.',
              style: theme.textTheme.bodyMedium?.copyWith(
                color: theme.colorScheme.onSurfaceVariant,
              ),
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 24),
            FilledButton.icon(
              onPressed: onRetry,
              icon: const Icon(Icons.refresh),
              label: const Text('Refresh'),
            ),
          ],
        ),
      ),
    );
  }
}
