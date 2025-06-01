"""
The ddcutil output parser for the display_detect command.
"""

import sys
import json

def main(text):
    """
    This method extracts the display names of all monitors connected to the system.

    Args:
        text: The output of the ddcutil command pass through the dotnet runtime.

    Returns:
        A JSON array containing the names of each monitor.
    """

    res = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            if line.startswith('Model:'):
                model = line.split('Model:')[1].strip()
                res.append(model)
        except json.JSONDecodeError as e:
            print(f"Error decoding JSON: {e}")
    print(json.dumps(res))

if __name__ == "__main__":
    input_text = sys.stdin.read()
    main(input_text)
