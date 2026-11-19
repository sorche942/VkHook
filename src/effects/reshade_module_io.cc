#include "reshade_module_io.hh"

#include <cstring>
#include <exception>
#include <string>

namespace VkHook
{
    namespace
    {
        constexpr uint32_t kSpirvMagic = 0x07230203u;
        // Constants nest only through array initializers; real shaders use one level.
        constexpr int kMaxConstantDepth = 8;
        // Smallest encoded size of each element kind. A count is checked against
        // these before it sizes an allocation, so a corrupt length cannot request
        // gigabytes.
        constexpr size_t kMinConstantBytes = 16 * 4 + 4 + 4;
        constexpr size_t kMinAnnotationBytes = 21 + 4 + kMinConstantBytes;
        constexpr size_t kMinEntryPointBytes = 4 + 1 + 12;
        constexpr size_t kMinTextureBytes = 4 + 4 + 4 + 4 + 4 + 16 + 2;
        constexpr size_t kMinSamplerBytes = 12 + 8 + 4 + 1 + 16 + 12 + 2;
        constexpr size_t kMinUniformBytes = 4 + 21 + 8 + 4 + 1 + kMinConstantBytes;
        constexpr size_t kMinTechniqueBytes = 4 + 4 + 4;
        constexpr size_t kMinPassBytes = 11 * 4 + 12;

        class Writer
        {
        public:
            std::vector<uint8_t> bytes;

            void u8(uint8_t value) { bytes.push_back(value); }
            void u32(uint32_t value)
            {
                for (int shift = 0; shift < 32; shift += 8)
                    bytes.push_back(static_cast<uint8_t>(value >> shift));
            }
            void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
            void f32(float value)
            {
                uint32_t bits;
                std::memcpy(&bits, &value, sizeof(bits));
                u32(bits);
            }
            void boolean(bool value) { u8(value ? 1 : 0); }
            void str(const std::string& value)
            {
                u32(static_cast<uint32_t>(value.size()));
                bytes.insert(bytes.end(), value.begin(), value.end());
            }
        };

        // Every read is bounds-checked and failure is sticky, so a decode routine
        // can read a whole struct and check `ok` once.
        class Reader
        {
        public:
            Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

            bool   ok() const { return m_ok; }
            size_t remaining() const { return m_size - m_pos; }

            uint8_t u8()
            {
                if (!take(1))
                    return 0;
                return m_data[m_pos - 1];
            }
            uint32_t u32()
            {
                if (!take(4))
                    return 0;
                const uint8_t* p = m_data + m_pos - 4;
                return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            }
            int32_t i32() { return static_cast<int32_t>(u32()); }
            float   f32()
            {
                const uint32_t bits = u32();
                float          value;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            }
            bool boolean() { return u8() != 0; }

            std::string str()
            {
                const uint32_t length = u32();
                if (!m_ok || length > remaining())
                {
                    m_ok = false;
                    return {};
                }
                std::string value(reinterpret_cast<const char*>(m_data + m_pos), length);
                m_pos += length;
                return value;
            }

            // An element count that cannot possibly fit in what is left, given that
            // every element takes at least `minBytes`, is corruption: reject it
            // before it is used to size an allocation.
            uint32_t count(size_t minBytes)
            {
                const uint32_t n = u32();
                if (m_ok && static_cast<uint64_t>(n) * minBytes > remaining())
                    m_ok = false;
                return m_ok ? n : 0;
            }

            void fail() { m_ok = false; }

        private:
            bool take(size_t n)
            {
                if (!m_ok || n > remaining())
                {
                    m_ok = false;
                    return false;
                }
                m_pos += n;
                return true;
            }

            const uint8_t* m_data;
            size_t         m_size;
            size_t         m_pos = 0;
            bool           m_ok = true;
        };

        // ---- type / constant / annotation ----

        void writeType(Writer& w, const reshadefx::type& t)
        {
            w.u8(static_cast<uint8_t>(t.base));
            w.u32(t.rows);
            w.u32(t.cols);
            w.u32(t.qualifiers);
            w.i32(t.array_length);
            w.u32(t.definition);
        }

        void readType(Reader& r, reshadefx::type& t)
        {
            const uint8_t base = r.u8();
            if (base > reshadefx::type::t_function)
                r.fail();
            t.base = static_cast<reshadefx::type::datatype>(base);
            t.rows = r.u32();
            t.cols = r.u32();
            t.qualifiers = r.u32();
            t.array_length = r.i32();
            t.definition = r.u32();
        }

        void writeConstant(Writer& w, const reshadefx::constant& c)
        {
            for (uint32_t word : c.as_uint)
                w.u32(word);
            w.str(c.string_data);
            w.u32(static_cast<uint32_t>(c.array_data.size()));
            for (const auto& element : c.array_data)
                writeConstant(w, element);
        }

        void readConstant(Reader& r, reshadefx::constant& c, int depth)
        {
            for (uint32_t& word : c.as_uint)
                word = r.u32();
            c.string_data = r.str();
            // A nested constant is at least 16 words + 2 lengths.
            const uint32_t n = r.count(kMinConstantBytes);
            if (depth >= kMaxConstantDepth && n != 0)
                r.fail();
            if (!r.ok())
                return;
            c.array_data.resize(n);
            for (auto& element : c.array_data)
            {
                readConstant(r, element, depth + 1);
                if (!r.ok())
                    return;
            }
        }

        void writeAnnotations(Writer& w, const std::vector<reshadefx::annotation>& list)
        {
            w.u32(static_cast<uint32_t>(list.size()));
            for (const auto& a : list)
            {
                writeType(w, a.type);
                w.str(a.name);
                writeConstant(w, a.value);
            }
        }

        void readAnnotations(Reader& r, std::vector<reshadefx::annotation>& list)
        {
            const uint32_t n = r.count(kMinAnnotationBytes);
            if (!r.ok())
                return;
            list.resize(n);
            for (auto& a : list)
            {
                readType(r, a.type);
                a.name = r.str();
                readConstant(r, a.value, 0);
                if (!r.ok())
                    return;
            }
        }

        // ---- module parts ----

        void writeTexture(Writer& w, const reshadefx::texture_info& t)
        {
            w.u32(t.id);
            w.u32(t.binding);
            w.str(t.semantic);
            w.str(t.unique_name);
            writeAnnotations(w, t.annotations);
            w.u32(t.width);
            w.u32(t.height);
            w.u32(t.depth);
            w.u32(t.levels);
            w.u8(t.dimensions);
            w.u8(static_cast<uint8_t>(t.format));
        }

        void readTexture(Reader& r, reshadefx::texture_info& t)
        {
            t.id = r.u32();
            t.binding = r.u32();
            t.semantic = r.str();
            t.unique_name = r.str();
            readAnnotations(r, t.annotations);
            t.width = r.u32();
            t.height = r.u32();
            t.depth = r.u32();
            t.levels = r.u32();
            t.dimensions = r.u8();
            const uint8_t format = r.u8();
            if (format > static_cast<uint8_t>(reshadefx::texture_format::r32u))
                r.fail();
            t.format = static_cast<reshadefx::texture_format>(format);
        }

        void writeSampler(Writer& w, const reshadefx::sampler_info& s)
        {
            w.u32(s.id);
            w.u32(s.binding);
            w.u32(s.texture_binding);
            w.str(s.unique_name);
            w.str(s.texture_name);
            writeAnnotations(w, s.annotations);
            w.u8(s.dimensions);
            w.u32(static_cast<uint32_t>(s.filter));
            w.u32(static_cast<uint32_t>(s.address_u));
            w.u32(static_cast<uint32_t>(s.address_v));
            w.u32(static_cast<uint32_t>(s.address_w));
            w.f32(s.min_lod);
            w.f32(s.max_lod);
            w.f32(s.lod_bias);
            w.u8(s.srgb);
            w.boolean(s.storage);
        }

        void readSampler(Reader& r, reshadefx::sampler_info& s)
        {
            s.id = r.u32();
            s.binding = r.u32();
            s.texture_binding = r.u32();
            s.unique_name = r.str();
            s.texture_name = r.str();
            readAnnotations(r, s.annotations);
            s.dimensions = r.u8();
            s.filter = static_cast<reshadefx::texture_filter>(r.u32());
            s.address_u = static_cast<reshadefx::texture_address_mode>(r.u32());
            s.address_v = static_cast<reshadefx::texture_address_mode>(r.u32());
            s.address_w = static_cast<reshadefx::texture_address_mode>(r.u32());
            s.min_lod = r.f32();
            s.max_lod = r.f32();
            s.lod_bias = r.f32();
            s.srgb = r.u8();
            s.storage = r.boolean();
        }

        void writeUniform(Writer& w, const reshadefx::uniform_info& u)
        {
            w.str(u.name);
            writeType(w, u.type);
            w.u32(u.size);
            w.u32(u.offset);
            writeAnnotations(w, u.annotations);
            w.boolean(u.has_initializer_value);
            writeConstant(w, u.initializer_value);
        }

        void readUniform(Reader& r, reshadefx::uniform_info& u)
        {
            u.name = r.str();
            readType(r, u.type);
            u.size = r.u32();
            u.offset = r.u32();
            readAnnotations(r, u.annotations);
            u.has_initializer_value = r.boolean();
            readConstant(r, u.initializer_value, 0);
        }

        void writePass(Writer& w, const reshadefx::pass_info& p)
        {
            for (const auto& name : p.render_target_names)
                w.str(name);
            w.str(p.vs_entry_point);
            w.str(p.ps_entry_point);
            w.str(p.cs_entry_point);
            w.u32(p.compute_group_size_x);
            w.u32(p.compute_group_size_y);
            w.u32(p.compute_group_size_z);
            w.u32(p.dispatch_size_x);
            w.u32(p.dispatch_size_y);
            w.u32(p.dispatch_size_z);
            w.u8(p.clear_render_targets);
            w.u8(p.srgb_write_enable);
            w.u8(p.blend_enable);
            w.u8(p.stencil_enable);
            w.u8(p.color_write_mask);
            w.u8(p.stencil_read_mask);
            w.u8(p.stencil_write_mask);
            w.u8(static_cast<uint8_t>(p.blend_op));
            w.u8(static_cast<uint8_t>(p.blend_op_alpha));
            w.u8(static_cast<uint8_t>(p.src_blend));
            w.u8(static_cast<uint8_t>(p.dest_blend));
            w.u8(static_cast<uint8_t>(p.src_blend_alpha));
            w.u8(static_cast<uint8_t>(p.dest_blend_alpha));
            w.u8(static_cast<uint8_t>(p.stencil_comparison_func));
            w.u32(p.stencil_reference_value);
            w.u8(static_cast<uint8_t>(p.stencil_op_pass));
            w.u8(static_cast<uint8_t>(p.stencil_op_fail));
            w.u8(static_cast<uint8_t>(p.stencil_op_depth_fail));
            w.u32(p.num_vertices);
            w.u8(static_cast<uint8_t>(p.topology));
            w.u32(p.viewport_width);
            w.u32(p.viewport_height);
        }

        void readPass(Reader& r, reshadefx::pass_info& p)
        {
            for (auto& name : p.render_target_names)
                name = r.str();
            p.vs_entry_point = r.str();
            p.ps_entry_point = r.str();
            p.cs_entry_point = r.str();
            p.compute_group_size_x = r.u32();
            p.compute_group_size_y = r.u32();
            p.compute_group_size_z = r.u32();
            p.dispatch_size_x = r.u32();
            p.dispatch_size_y = r.u32();
            p.dispatch_size_z = r.u32();
            p.clear_render_targets = r.u8();
            p.srgb_write_enable = r.u8();
            p.blend_enable = r.u8();
            p.stencil_enable = r.u8();
            p.color_write_mask = r.u8();
            p.stencil_read_mask = r.u8();
            p.stencil_write_mask = r.u8();
            p.blend_op = static_cast<reshadefx::pass_blend_op>(r.u8());
            p.blend_op_alpha = static_cast<reshadefx::pass_blend_op>(r.u8());
            p.src_blend = static_cast<reshadefx::pass_blend_func>(r.u8());
            p.dest_blend = static_cast<reshadefx::pass_blend_func>(r.u8());
            p.src_blend_alpha = static_cast<reshadefx::pass_blend_func>(r.u8());
            p.dest_blend_alpha = static_cast<reshadefx::pass_blend_func>(r.u8());
            p.stencil_comparison_func = static_cast<reshadefx::pass_stencil_func>(r.u8());
            p.stencil_reference_value = r.u32();
            p.stencil_op_pass = static_cast<reshadefx::pass_stencil_op>(r.u8());
            p.stencil_op_fail = static_cast<reshadefx::pass_stencil_op>(r.u8());
            p.stencil_op_depth_fail = static_cast<reshadefx::pass_stencil_op>(r.u8());
            p.num_vertices = r.u32();
            p.topology = static_cast<reshadefx::primitive_topology>(r.u8());
            p.viewport_width = r.u32();
            p.viewport_height = r.u32();
        }

        template <typename T, typename WriteOne>
        void writeList(Writer& w, const std::vector<T>& list, WriteOne writeOne)
        {
            w.u32(static_cast<uint32_t>(list.size()));
            for (const auto& element : list)
                writeOne(w, element);
        }

        template <typename T, typename ReadOne>
        void readList(Reader& r, std::vector<T>& list, size_t minBytes, ReadOne readOne)
        {
            const uint32_t n = r.count(minBytes);
            if (!r.ok())
                return;
            list.resize(n);
            for (auto& element : list)
            {
                readOne(r, element);
                if (!r.ok())
                    return;
            }
        }
    } // namespace

    std::vector<uint8_t> serializeReshadeModule(const reshadefx::module& module)
    {
        Writer w;
        w.str(module.hlsl);

        w.u32(static_cast<uint32_t>(module.spirv.size()));
        for (uint32_t word : module.spirv)
            w.u32(word);

        writeList(w, module.entry_points, [](Writer& out, const reshadefx::entry_point& e) {
            out.str(e.name);
            out.u8(static_cast<uint8_t>(e.stage));
            out.u32(e.local_size_x);
            out.u32(e.local_size_y);
            out.u32(e.local_size_z);
        });
        writeList(w, module.textures, writeTexture);
        writeList(w, module.samplers, writeSampler);
        writeList(w, module.uniforms, writeUniform);
        writeList(w, module.spec_constants, writeUniform);
        writeList(w, module.techniques, [](Writer& out, const reshadefx::technique_info& t) {
            out.str(t.name);
            writeList(out, t.passes, writePass);
            writeAnnotations(out, t.annotations);
        });

        w.u32(module.total_uniform_size);
        w.u32(module.num_sampler_bindings);
        w.u32(module.num_texture_bindings);
        return std::move(w.bytes);
    }

    bool deserializeReshadeModule(const uint8_t* data, size_t size, reshadefx::module& module)
    {
        try
        {
            Reader r(data, size);
            module = reshadefx::module{};
            module.hlsl = r.str();

            const uint32_t spirvWords = r.count(4);
            if (r.ok())
            {
                module.spirv.resize(spirvWords);
                for (uint32_t& word : module.spirv)
                    word = r.u32();
            }

            readList(r, module.entry_points, kMinEntryPointBytes, [](Reader& in, reshadefx::entry_point& e) {
                e.name = in.str();
                const uint8_t stage = in.u8();
                if (stage > static_cast<uint8_t>(reshadefx::entry_point_stage::compute))
                    in.fail();
                e.stage = static_cast<reshadefx::entry_point_stage>(stage);
                e.local_size_x = in.u32();
                e.local_size_y = in.u32();
                e.local_size_z = in.u32();
            });
            readList(r, module.textures, kMinTextureBytes, readTexture);
            readList(r, module.samplers, kMinSamplerBytes, readSampler);
            readList(r, module.uniforms, kMinUniformBytes, readUniform);
            readList(r, module.spec_constants, kMinUniformBytes, readUniform);
            readList(r, module.techniques, kMinTechniqueBytes, [](Reader& in, reshadefx::technique_info& t) {
                t.name = in.str();
                readList(in, t.passes, kMinPassBytes, readPass);
                readAnnotations(in, t.annotations);
            });

            module.total_uniform_size = r.u32();
            module.num_sampler_bindings = r.u32();
            module.num_texture_bindings = r.u32();

            if (!r.ok() || r.remaining() != 0)
                return false;
            return module.spirv.empty() || module.spirv[0] == kSpirvMagic;
        }
        catch (const std::exception&)
        {
            // Only an allocation failure can land here; a cache read is optional.
            return false;
        }
    }
} // namespace VkHook
