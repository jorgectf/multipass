import 'package:desktop_gui/grpc_client.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

void main() {
  runApp(const ProviderScope(child: MaterialApp(home: MyHomePage())));
}

class MyHomePage extends ConsumerWidget {
  const MyHomePage({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final infos = ref.watch(vmStatusesProvider);
    return Scaffold(
      body: Row(children: [
        for (final MapEntry(key: name, value: status) in infos.entries)
          Text('$name $status')
      ]),
    );
  }
}
