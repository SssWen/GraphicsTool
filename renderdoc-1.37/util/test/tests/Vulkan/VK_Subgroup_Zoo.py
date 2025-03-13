import renderdoc as rd
import struct
import rdtest


class VK_Subgroup_Zoo(rdtest.TestCase):
    demos_test_name = 'VK_Subgroup_Zoo'

    def check_support(self, **kwargs):
        # Only allow this if explicitly run
        if kwargs['test_include'] == 'VK_Subgroup_Zoo':
            return True, ''
        return False, 'Disabled test'

    def check_capture(self):
        graphics_tests = [a for a in self.find_action(
            "Graphics Tests").children if a.flags & rd.ActionFlags.Drawcall]
        compute_dims = [a for a in self.find_action(
            "Compute Tests").children if 'x' in a.customName]

        rdtest.log.begin_section("Graphics tests")

        # instances to check in instanced draws
        inst_checks = [0, 1, 5, 10]
        # pixels to check
        pixel_checks = [
            # top quad
            (0, 0), (1, 0), (0, 1), (1, 1),
            # middle quad (away from triangle border)
            (64, 56), (65, 56), (64, 57), (65, 57),
            # middle quad (on triangle border)
            (64, 64), (65, 64), (64, 65), (65, 65),
            # middle quad on other triangle
            (56, 64), (57, 64), (56, 65), (57, 65),
        ]
        # threads to check. largest dimension only (all small dim checked)
        thread_checks = [
            # first few
            0, 1, 2,
            # near end of 32-subgroup and boundary
            30, 31, 32,
            # near end of 64-subgroup and boundary
            62, 63, 64,
            # near end of 64-subgroup and boundary
            62, 63, 64,
            # large values spaced out with one near the end of our unaligned size
            100, 110, 120, 140, 149, 150, 160, 200, 250,
        ]
        clear_col = (123456.0, 789.0, 101112.0, 0.0)

        for idx, action in enumerate(graphics_tests):
            self.controller.SetFrameEvent(action.eventId, False)

            pipe = self.controller.GetPipelineState()

            # check vertex output for every vertex
            for inst in [inst for inst in inst_checks if inst < action.numInstances]:
                for view in range(pipe.MultiviewBroadcastCount()):

                    postvs = self.get_postvs(
                        action, rd.MeshDataStage.VSOut, first_index=0, num_indices=action.numIndices, instance=inst)

                    for vtx in range(action.numIndices):
                        trace = self.controller.DebugVertex(
                            vtx, inst, vtx, view)

                        if trace.debugger is None:
                            self.controller.FreeTrace(trace)

                            rdtest.log.error(
                                f"Test {test} at {action.eventId} got no debug result at {vtx} inst {inst} view {view}")
                            return

                        _, variables = self.process_trace(trace)

                        for var in trace.sourceVars:
                            if var.name == 'vertdata':
                                name = var.name

                                if var.name not in postvs[vtx].keys():
                                    rdtest.log.error(
                                        f"Don't have expected output for {var.name}")
                                    continue

                                real = postvs[vtx][name]
                                debugged = self.evaluate_source_var(
                                    var, variables)

                                if debugged.columns != 4 or len(real) != 4:
                                    rdtest.log.error(
                                        f"Vertex output is not the right size ({len(real)} vs {debugged.columns})")
                                    continue

                                if not rdtest.value_compare(real, debugged.value.f32v[0:4], eps=5.0E-06):
                                    rdtest.log.error(
                                        f"Test {idx} debugged vertex value {debugged.value.f32v[0:4]} at {vtx} instance {inst} view {view} does not match output {real}")

                        self.controller.FreeTrace(trace)

            # check some assorted pixel outputs
            target = pipe.GetOutputTargets()[0].resource

            for pixel in pixel_checks:
                for view in range(pipe.MultiviewBroadcastCount()):
                    x, y = pixel

                    picked = self.controller.PickPixel(
                        target, x, y, rd.Subresource(0, 0, 0), rd.CompType.Float)

                    real = picked.floatValue

                    # silently skip pixels that weren't written to
                    if real == clear_col:
                        continue

                    inputs = rd.DebugPixelInputs()
                    inputs.sample = 0
                    inputs.primitive = rd.ReplayController.NoPreference
                    inputs.view = view
                    trace = self.controller.DebugPixel(x, y, inputs)

                    if trace.debugger is None:
                        self.controller.FreeTrace(trace)

                        rdtest.log.error(
                            f"Test {test} at {action.eventId} got no debug result at {x},{y}")
                        continue

                    _, variables = self.process_trace(trace)

                    output_sourcevar = self.find_output_source_var(
                        trace, rd.ShaderBuiltin.ColorOutput, 0)

                    if output_sourcevar is None:
                        rdtest.log.error("No output variable found")
                        continue

                    debugged = self.evaluate_source_var(
                        output_sourcevar, variables)

                    self.controller.FreeTrace(trace)

                    debuggedValue = list(debugged.value.f32v[0:4])

                    if not rdtest.value_compare(real, debuggedValue, eps=5.0E-06):
                        rdtest.log.error(
                            f"Test {idx} debugged pixel value {debuggedValue} at {x},{y} in {view} does not match output {real}")

            rdtest.log.success(f"Test {idx} successful")

        rdtest.log.end_section("Graphics tests")

        for comp_dim in compute_dims:
            rdtest.log.begin_section(
                f"Compute tests with {comp_dim.customName} workgroup")

            compute_tests = [
                a for a in comp_dim.children if a.flags & rd.ActionFlags.Dispatch]

            for test, action in enumerate(compute_tests):
                self.controller.SetFrameEvent(action.eventId, False)

                pipe = self.controller.GetPipelineState()
                csrefl = pipe.GetShaderReflection(rd.ShaderStage.Compute)

                dim = csrefl.dispatchThreadsDimension

                rw = pipe.GetReadWriteResources(rd.ShaderStage.Compute)

                if len(rw) != 1:
                    rdtest.log.error("Unexpected number of RW resources")
                    continue

                # each test writes up to 16k data, one vec4 per thread * up to 1024 threads
                bufdata = self.controller.GetBufferData(
                    rw[0].descriptor.resource, test*16*1024, 16*1024)

                for t in thread_checks:
                    xrange = 1
                    yrange = dim[1]
                    xbase = t
                    ybase = 0

                    # vertical orientation
                    if dim[1] > dim[0]:
                        xrange = dim[0]
                        yrange = 1
                        xbase = 0
                        ybase = t

                    for x in range(xbase, xbase+xrange):
                        for y in range(ybase, ybase+yrange):
                            z = 0

                            if x >= dim[0] or y >= dim[1]:
                                continue

                            real = struct.unpack_from(
                                "4f", bufdata, 16*y*dim[0] + 16*x)

                            trace = self.controller.DebugThread(
                                (0, 0, 0), (x, y, z))

                            _, variables = self.process_trace(trace)

                            if trace.debugger is None:
                                self.controller.FreeTrace(trace)

                                rdtest.log.error(
                                    f"Test {test} at {action.eventId} got no debug result at {x},{y},{z}")
                                continue

                            sourceVars = [
                                v for v in trace.instInfo[-1].sourceVars if v.name == 'data']

                            if len(sourceVars) != 1:
                                rdtest.log.error(
                                    "Couldn't find compute data variable")
                                continue

                            debugged = self.evaluate_source_var(
                                sourceVars[0], variables)

                            debuggedValue = list(debugged.value.f32v[0:4])

                            if not rdtest.value_compare(real, debuggedValue, eps=5.0E-06):
                                rdtest.log.error(
                                    f"Test {test} at {action.eventId} debugged thread value {debuggedValue} at {x},{y},{z} does not match output {real}")

                rdtest.log.success(f"Test {test} successful")

            rdtest.log.end_section(
                f"Compute tests with {comp_dim.customName} workgroup")
