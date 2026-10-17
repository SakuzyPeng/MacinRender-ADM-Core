"""Validate the complete version-1 AdmScene word stream emitted by scene_checkpoint.h.

This checks the encoding, not ADM semantics or floating-point values. The latter may include
signed zero and other bit patterns that must survive comparison unchanged.
"""

import struct


RENDER_SCENE_STAGES = ('05-import', '06-policy')


class _Reader:
    def __init__(self, data):
        if not data or len(data) % 4:
            raise ValueError('empty or truncated render-scene checkpoint')
        self.data = data
        self.offset = 0

    def words(self, count):
        start = self.offset
        if count > (len(self.data) - start) // 4:
            raise ValueError(f'truncated render-scene checkpoint at word {start // 4}')
        self.offset += 4 * count
        return start

    def u32(self):
        return struct.unpack_from('<I', self.data, self.words(1))[0]

    def u16(self):
        if self.u32() > 0xffff:
            raise ValueError('invalid render-scene channel index')

    def u64(self):
        self.words(2)

    def boolean(self):
        value = self.u32()
        if value not in (0, 1):
            raise ValueError(f'invalid render-scene boolean at word {self.offset // 4 - 1}')
        return value

    def optional(self, read):
        if self.boolean():
            read()

    def sequence(self, read):
        count = self.u32()
        # Every encoded element consumes at least one word. Bound even corrupt counts before
        # iterating, without allocating from untrusted lengths.
        if count > (len(self.data) - self.offset) // 4:
            raise ValueError('truncated render-scene list')
        for _ in range(count):
            read()

    def text(self):
        size = self.u32()
        start = self.words((size + 3) // 4)
        try:
            self.data[start:start + size].decode('utf-8')
        except UnicodeDecodeError as error:
            raise ValueError('invalid render-scene UTF-8 string') from error
        if any(self.data[start + size:self.offset]):
            raise ValueError('nonzero render-scene string padding')

    def texts(self):
        self.sequence(self.text)

    def position(self):
        self.boolean()  # cartesian
        self.words(6)  # polar and Cartesian f32 coordinates

    def gain_source(self):
        self.boolean()  # present
        self.boolean()  # decibels
        self.words(3)  # f64 value, f32 linear

    def block_source(self):
        self.gain_source()
        self.boolean()  # rtime_present
        self.u64()
        self.optional(self.u64)  # duration_samples

    def user_level(self):
        self.u32()  # f32 gain_multiplier
        self.optional(self.boolean)  # mute

    def object_block(self):
        self.position()
        self.words(9)  # five f32 values, u64 start/end
        self.boolean()  # jump_position
        self.optional(self.u64)  # interp_length_samples
        self.boolean()  # channel_lock
        self.optional(self.u32)  # channel_lock_max_distance
        self.words(3)  # divergence and its ranges
        self.boolean()  # screen_ref
        self.boolean()  # head_locked
        self.optional(self.block_source)

    def direct_block(self):
        self.texts()  # speaker_labels
        self.text()  # pack_format_id
        self.words(3)  # azimuth/elevation/distance
        self.boolean()  # has_position
        for _ in range(6):
            self.optional(self.u32)  # position range hints
        self.u32()  # gain
        self.optional(self.u32)  # low_pass_hz
        self.words(4)  # start/end
        self.boolean()  # head_locked
        self.optional(self.block_source)
        self.optional(self.position)  # source_position
        self.user_level()
        self.boolean()  # user_position_override

    def loudness(self):
        for _ in range(6):
            self.optional(self.u32)
        self.optional(self.text)  # method

    def screen(self):
        self.boolean()  # cartesian
        self.words(10)  # five f64 values: aspect ratio, centre, width

    def track(self):
        self.optional(self.u16)  # channel_index
        self.text()  # track_uid
        self.sequence(self.object_block)
        self.sequence(self.direct_block)

    def object_source(self):
        self.gain_source()
        self.boolean()  # mute_present
        self.boolean()  # mute
        self.boolean()  # start_present
        self.words(4)  # start_samples, absolute_start_samples
        self.optional(self.u64)  # duration_samples
        self.texts()  # child_objects
        self.boolean()  # has_parent

    def object(self):
        self.text()  # id
        self.text()  # name
        self.u32()  # gain
        self.boolean()  # mute
        self.u64()  # end_sample
        self.optional(self.position)  # position_offset has the same encoding as position
        self.sequence(self.track)
        self.texts()  # labels
        self.optional(self.u32)  # importance
        self.optional(self.u32)  # dialogue_id
        self.boolean()  # head_locked
        self.optional(self.object_source)
        self.user_level()

    def programme(self):
        self.text()  # id
        self.text()  # name
        self.texts()  # content_ids
        self.optional(self.text)  # language
        self.texts()  # labels
        self.u64()  # start_sample
        self.optional(self.u64)  # end_sample
        self.optional(self.loudness)
        self.boolean()  # has_reference_screen
        self.optional(self.screen)

    def content(self):
        self.text()  # id
        self.text()  # name
        self.texts()  # object_ids
        self.optional(self.text)  # language
        self.texts()  # labels
        self.optional(self.loudness)
        self.optional(self.text)  # dialogue_kind
        self.optional(self.text)  # content_kind

    def hoa_block(self):
        self.words(5)  # f32 gain, u64 start/end
        self.boolean()  # head_locked

    def hoa_channel(self):
        self.optional(self.u16)  # channel_index
        self.text()  # track_uid
        self.words(2)  # signed order/degree
        self.sequence(self.hoa_block)

    def hoa_tracks(self):
        self.text()  # object_id
        self.text()  # pack_format_id
        self.text()  # normalization
        self.u64()  # f64 nfc_ref_dist
        self.boolean()  # screen_ref
        self.u32()  # gain
        self.boolean()  # mute
        self.words(4)  # start/end
        self.sequence(self.hoa_channel)
        self.boolean()  # head_locked


def validate_render_scene(data):
    reader = _Reader(data)
    if reader.u32() != 1:
        raise ValueError('unknown render-scene checkpoint encoding')
    reader.u32()  # sample_rate
    reader.u16()  # num_channels
    reader.u64()  # num_frames
    if reader.u32() not in (0, 1):
        raise ValueError('invalid render-scene source kind')
    reader.text()  # input_layout
    reader.texts()  # input_channel_labels
    reader.sequence(reader.programme)
    reader.sequence(reader.content)
    reader.sequence(reader.object)
    reader.sequence(reader.hoa_tracks)
    reader.u32()  # only the warning count is encoded, not the warning strings
    if reader.offset != len(data):
        raise ValueError('trailing words in render-scene checkpoint')


def require_render_scene_checkpoints(root, case_id):
    for stage in RENDER_SCENE_STAGES:
        path = root / 'render-scene' / f'offline.{stage}.i32'
        if not path.is_file():
            raise ValueError(f'missing render-scene checkpoint {stage}: {case_id}')
        try:
            validate_render_scene(path.read_bytes())
        except ValueError as error:
            raise ValueError(f'{error}: {stage}: {case_id}') from error
