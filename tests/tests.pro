TEMPLATE = subdirs

# Unit tests. Build and run everything with:
#   cd tests && qmake6 && make -j$(nproc) && make check
SUBDIRS += \
    bus_message_signal \
    bus_message_frame \
    can_db_signal \
    dbc_parser \
    can_db \
    trace_file_format \
    trace_line_format \
    pcapng_format \
    trace_file_writer \
    trace_recorder \
    decoders \
    autosar_e2e \
    ldf_parser \
    slcan_codec
