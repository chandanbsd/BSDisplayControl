import sys
import json
import re

def main(text):
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