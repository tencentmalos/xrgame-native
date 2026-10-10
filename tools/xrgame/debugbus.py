#!/usr/bin/env python3
"""Query the internal XRGame Foundation DebugBus through adb, without TCP forwarding."""
import argparse
import json
import subprocess
import sys

COMPONENT = 'com.tencentmalos.xrgamenative/app.gamenative.xrgame.DebugBusService'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True, help='Explicit adb device serial')
    parser.add_argument('--start', action='store_true', help='Start service; bring XRGame to foreground first')
    parser.add_argument('--stop', action='store_true', help='Stop only DebugBusService')
    parser.add_argument('request', nargs='*', default=['help'])
    args = parser.parse_args()
    adb = ['adb', '-s', args.serial, 'shell']
    try:
        if args.stop:
            result = subprocess.run(adb + ['am', 'stopservice', '-n', COMPONENT],
                                    capture_output=True, text=True, timeout=10)
            response = result.stdout + result.stderr
            print(response, end='')
            # AYN Android 13 returns 255 with "Service stopped" on stderr even on success.
            # Treat the explicit framework result as authoritative; missing/error output fails.
            return 0 if any(line.strip() in ('Service stopped', 'Service not stopped: was not running.')
                            for line in response.splitlines()) else 1
        if args.start:
            result = subprocess.run(adb + ['am', 'startservice', '-n', COMPONENT], check=True,
                                    capture_output=True, text=True, timeout=10)
            if 'Error' in result.stdout + result.stderr:
                sys.stderr.write(result.stdout + result.stderr)
                return 1
        request = args.request or ['help']
        # adb shell joins remote arguments. The protocol only accepts identifiers, integers and
        # key=value pairs (values may be relative names such as s0/autosave); reject shell
        # metacharacters rather than passing arbitrary code through adb.
        if len(request) > 5 or any(len(s) > 64 or not s or s[0] in '=/' or
                                 any(not (c.isascii() and (c.isalnum() or c in '._-=/')) for c in s)
                                 for s in request):
            parser.error('Use command identifiers, integers or key=value arguments (up to 5 tokens, 64 characters each)')
        result = subprocess.run(adb + ['dumpsys', 'activity', 'service', COMPONENT, *request],
                                capture_output=True, text=True, check=True, timeout=10)
        lines = result.stdout.splitlines()
        # Android prefixes service identity and indents the dump body. Preserve all response lines
        # when requesting help, but expose machine-readable JSON without framework decoration.
        bodies = [line.strip() for line in lines if line.lstrip().startswith('{')]
        if bodies:
            payload = json.loads(bodies[-1])
            print(json.dumps(payload, ensure_ascii=False, indent=2))
            return 1 if 'error' in payload else 0
        if request[0] in ('profiler_ring', 'profiler_capture'):
            fields = dict(line.strip().split('=', 1) for line in lines if '=' in line
                          and not line.lstrip().startswith('SERVICE '))
            if fields.get('schema', '').startswith('foundation.profiler-'):
                print(json.dumps(fields, ensure_ascii=False, indent=2))
                return 1 if fields.get('error') else 0
        print(result.stdout, end='')
        return 0 if any('Available requests:' in line for line in lines) and request[0] in ('help', '--help', '-h') else 1
    except (subprocess.SubprocessError, OSError, ValueError) as error:
        print(f'DebugBus query failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
