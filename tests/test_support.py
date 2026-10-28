"""Shared compiler-test utilities."""
import re
import struct
import subprocess

TARGETS = ('glsl410', 'glsl430', 'glsl300es', 'glsl310es', 'hlsl4', 'hlsl5',
           'metal_macos', 'metal_ios', 'metal_sim', 'wgsl', 'spirv_vk')


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def rewrite_spirv(data, *, strip_names=False, entry=None, workgroup=None):
    words = list(struct.unpack('<' + 'I' * (len(data) // 4), data))
    output = words[:5]
    pos = 5
    while pos < len(words):
        count, op = words[pos] >> 16, words[pos] & 0xffff
        instruction = words[pos:pos + count]
        if workgroup is not None and op == 16 and instruction[2] == 17:  # OpExecutionMode LocalSize
            instruction[3:6] = workgroup
        if workgroup is not None and op == 71 and instruction[2:] == [11, 25]:
            # Drop BuiltIn WorkgroupSize, which otherwise overrides LocalSize.
            instruction = []
        if strip_names and op in (5, 6):  # OpName, OpMemberName
            instruction = []
        if entry and op == 15:  # OpEntryPoint
            name_end = 3
            while 0 not in struct.pack('<I', instruction[name_end]):
                name_end += 1
            encoded = entry.encode() + b'\0'
            encoded += b'\0' * (-len(encoded) % 4)
            name = list(struct.unpack('<' + 'I' * (len(encoded) // 4), encoded))
            instruction = instruction[:3] + name + instruction[name_end + 1:]
            instruction[0] = len(instruction) << 16 | op
        output += instruction
        pos += count
    return struct.pack('<' + 'I' * len(output), *output)


def decode_source(data):
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', data)).rstrip(b'\0')


def check_translation(glslang, header, symbol, target, stage, work):
    match = re.search(re.escape(symbol + '_source_' + target) + r'\[\d+\] = \{(.*?)\};',
                      header.read_text(), re.S)
    assert match, f'missing {target} source: {symbol}'
    source = work / ('translated.' + stage)
    source.write_bytes(decode_source(match[1]))
    flags = ['-D', '-V', '-e', 'main', '-o', str(work / 'hlsl.spv')] if target.startswith('hlsl') else []
    run([glslang, *flags, '-S', stage, str(source)])
