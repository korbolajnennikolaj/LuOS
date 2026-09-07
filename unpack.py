import os
import re

def unpack_source(filename="fullsource.txt"):
    if not os.path.exists(filename):
        print(f"Error: {filename} not found!")
        return

    file_pattern = re.compile(
        r"={20,}\n\s*FILE:\s*(.*?)\s*\n={20,}",
        re.MULTILINE
    )

    with open(filename, 'r', encoding='utf-8') as f:
        content = f.read()

    matches = list(file_pattern.finditer(content))

    for i, match in enumerate(matches):
        file_path = match.group(1).strip()

        start_pos = match.end()
        end_pos = matches[i+1].start() if i + 1 < len(matches) else len(content)

        file_content = content[start_pos:end_pos].strip()

        directory = os.path.dirname(file_path)
        if directory and not os.path.exists(directory):
            os.makedirs(directory)

        try:
            with open(file_path, 'w', encoding='utf-8') as out_file:
                out_file.write(file_content)
            print(f"  [OK] Unpacked: {file_path}")
        except Exception as e:
            print(f"  [ERR] Failed to write {file_path}: {e}")

if __name__ == "__main__":
    print("Starting to unpack fullsource.txt...")
    unpack_source()
    print("Unpacking complete.")
