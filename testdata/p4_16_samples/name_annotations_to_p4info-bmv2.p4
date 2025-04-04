/*
Copyright 2021 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <core.p4>
#include <v1model.p4>

struct H { };
struct M {
    bit<32> hash1;
}

parser ParserI(packet_in pk, out H hdr, inout M meta, inout standard_metadata_t smeta) {
    state start { transition accept; }
}

@name("altIngressName")
control IngressI(inout H hdr, inout M meta, inout standard_metadata_t smeta) {
    @name("blue")
    action foo(@name("orange") bit<9> port) {
        smeta.egress_spec = port;
    }

    @name("red")
    table foo_table {
        key = {
            smeta.ingress_port : exact @name("green");
        }
        actions = {
            @name("yellow") foo;
            @name("grey") NoAction;
        }
        const default_action = NoAction();
    }
    apply {
        foo_table.apply();
    }
};

control EgressI(inout H hdr, inout M meta, inout standard_metadata_t smeta) {
    apply { }
};

control DeparserI(packet_out pk, in H hdr) {
    apply { }
}

control VerifyChecksumI(inout H hdr, inout M meta) {
    apply { }
}

control ComputeChecksumI(inout H hdr, inout M meta) {
    apply { }
}

V1Switch(ParserI(), VerifyChecksumI(), IngressI(), EgressI(),
         ComputeChecksumI(), DeparserI()) main;
