# SPDX-License-Identifier: BSD-3-Clause
dnl The OEM DAP module accepts stereo input and produces four speaker channels.
STREAM_SG_PCM_ADD(audioreach/subgraph-stream-vol-playback.m4, FRONTEND_DAI_MULTIMEDIA4,
	`S16_LE', 48000, 48000, 2, 2,
	0x0000400B, 0x0000400B, 0x00006100, `110000')

SectionVendorTuples."stream3.dap1_tuples" {
        tokens "audioreach_tokens"
        tuples."word.u32_data" {
                AR_TKN_U32_MODULE_INSTANCE_ID "PIANO_DAP_INSTANCE"
                AR_TKN_U32_MODULE_ID "0x100108db"
                AR_TKN_U32_MODULE_MAX_IP_PORTS "1"
                AR_TKN_U32_MODULE_MAX_OP_PORTS "1"
                AR_TKN_U32_MODULE_SRC_OP_PORT_ID "1"
                AR_TKN_U32_MODULE_DST_IN_PORT_ID "2"
                AR_TKN_U32_MODULE_SRC_INSTANCE_ID "PIANO_DAP_INSTANCE"
                AR_TKN_U32_MODULE_DST_INSTANCE_ID "24837"
        }
}

SectionData."stream3.dap1_data" {
        tuples "stream3.dap1_tuples"
}

SectionData."stream3.dap1_config" {
        file "speaker-dap-config.bin"
}

SectionWidget."stream3.dap1" {
        index "1"
        type "src"
        no_pm "true"
        subseq "10"
        data [
                "stream3.sub_graph1_data"
                "stream3.container1_data"
                "stream3.dap1_data"
                "stream3.dap1_config"
        ]
}
