#include "Emboss.hpp"
#include <stdio.h>
#include <cstdlib>

#define STB_TRUETYPE_IMPLEMENTATION // force following include to generate implementation
#include "imgui/imstb_truetype.h" // stbtt_fontinfo

using namespace Slic3r;

// do not expose out of this file stbtt_ data types
class Privat
{
public:
    Privat() = delete;

    static std::optional<stbtt_fontinfo> load_font_info(const Emboss::Font &font);

    struct Glyph
    {
        Polygons polygons;
        int      advance_width, left_side_bearing;
    };
    static std::optional<Glyph> get_glyph(stbtt_fontinfo &font_info, int unicode_letter, float flatness = 2.f);
};

std::optional<stbtt_fontinfo> Privat::load_font_info(const Emboss::Font &font)
{
    int font_offset = stbtt_GetFontOffsetForIndex(font.buffer.data(), font.index);
    if (font_offset < 0) {
        std::cerr << "Font index("<<font.index<<") doesn't exist.";
        return {};        
    }
    stbtt_fontinfo font_info;
    if (stbtt_InitFont(&font_info, font.buffer.data(), font_offset) == 0) {
        std::cerr << "Can't initialize font.";
        return {};
    }
    return font_info;
}

std::optional<Privat::Glyph> Privat::get_glyph(stbtt_fontinfo &font_info, int unicode_letter, float flatness)
{
    int glyph_index = stbtt_FindGlyphIndex(&font_info, unicode_letter);
    if (glyph_index == 0) { 
        std::cerr << "Character codepoint(" << unicode_letter 
            << " = '" << (char) unicode_letter << "') is not defined in the font.";
        return {};
    }

    Privat::Glyph glyph;
    stbtt_GetGlyphHMetrics(&font_info, glyph_index, &glyph.advance_width, &glyph.left_side_bearing);

    stbtt_vertex *vertices;
    int num_verts = stbtt_GetGlyphShape(&font_info, glyph_index, &vertices);
    if (num_verts <= 0) return glyph; // no shape

    int *contour_lengths = NULL;
    int  num_countour    = 0;

    stbtt__point *points = stbtt_FlattenCurves(vertices, num_verts,
        flatness, &contour_lengths, &num_countour, font_info.userdata);

    glyph.polygons.reserve(num_countour);
    size_t pi = 0; // point index
    for (size_t ci = 0; ci < num_countour; ++ci) {
        int    length = contour_lengths[ci];
        // minimal length for triangle
        assert(length >= 4);
        // last point is first point
        --length;
        Points pts;
        pts.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            const stbtt__point &point = points[pi];
            ++pi;
            pts.emplace_back(point.x, point.y);
        }
        // last point is first point
        assert(pts.front() == Point(points[pi].x, points[pi].y));
        ++pi;
        glyph.polygons.emplace_back(pts);
    }

    // inner ccw
    // outer cw
    return glyph;
}

std::optional<Emboss::Font> Emboss::load_font(const char *file_path)
{
    FILE *file = fopen(file_path, "rb");
    if (file == nullptr) {
        std::cerr << "Couldn't open " << file_path << " for reading.";
        return {};
    }

    // find size of file
    if (fseek(file, 0L, SEEK_END) != 0) {
        std::cerr << "Couldn't fseek file " << file_path << " for size measure.";
        return {};
    }
    size_t size = ftell(file);
    if (size == 0) {
        std::cerr << "Size of font file is zero. Can't read.";
        return {};    
    }
    rewind(file);

    Font res;
    res.buffer = std::vector<unsigned char>(size);
    size_t count_loaded_bytes = fread((void *) &res.buffer.front(), 1, size, file);

    unsigned int index = 0;
    int font_offset = 0;
    while (font_offset >= 0) {
        font_offset = stbtt_GetFontOffsetForIndex(res.buffer.data(), index++);
    }
    // at least one font must be inside collection
    if (index < 1) {
        std::cerr << "There is no font collection inside file.";
        return {};        
    }
    // select default font on index 0
    res.index = 0;
    res.count = index;

    auto font_info = Privat::load_font_info(res);
    if (!font_info.has_value()) return {};

    // load information about line gap
    stbtt_GetFontVMetrics(&(*font_info), &res.ascent, &res.descent, &res.linegap);
    return res;
}

Polygons Emboss::letter2polygons(const Font &font, char letter, float flatness)
{
    auto font_info_opt = Privat::load_font_info(font);
    if (!font_info_opt.has_value()) return Polygons();
    stbtt_fontinfo *font_info = &(*font_info_opt);

    auto glyph_opt = Privat::get_glyph(*font_info_opt, (int) letter, flatness);
    if (!glyph_opt.has_value()) return Polygons();

    return glyph_opt->polygons;
}

#include <boost\nowide\convert.hpp>
Polygons Emboss::text2polygons(const Font &font, const char *text, float flatness)
{
    auto font_info_opt = Privat::load_font_info(font);
    if (!font_info_opt.has_value()) return Polygons();
    stbtt_fontinfo *font_info = &(*font_info_opt);

    Point    cursor(0, 0);
    Polygons result;

    std::wstring ws = boost::nowide::widen(text);
    for (wchar_t wc: ws){
        if (wc == '\n') { 
            cursor.x() = 0;
            cursor.y() -= font.ascent - font.descent + font.linegap;
            continue;
        } 
        int unicode = static_cast<int>(wc);
        auto glyph_opt = Privat::get_glyph(*font_info_opt, unicode, flatness);
        if (!glyph_opt.has_value()) continue;

        // move glyph to cursor position
        Polygons polygons = glyph_opt->polygons; // copy
        for (Polygon &polygon : polygons) 
            for (Point &p : polygon.points) p += cursor;
        
        cursor.x() += glyph_opt->advance_width;

        polygons_append(result, polygons);
    }
    return result;
}

indexed_triangle_set Emboss::polygons2model(const Polygons &shape2d,
                                            const IProject &projection)
{
    indexed_triangle_set result;
    size_t count_point = count_points(shape2d);
    result.vertices.reserve(2 * count_point);

    std::vector<Vec3f> &front_points = result.vertices;
    std::vector<Vec3f>  back_points;
    back_points.reserve(count_point);

    for (const Polygon &polygon : shape2d) {
        for (const Point &p : polygon.points) {
            auto p2 = projection.project(p);
            front_points.emplace_back(p2.first);
            back_points.emplace_back(p2.second);
        }
    }
    // insert back points, front are already in
    result.vertices.insert(result.vertices.end(),
                           std::make_move_iterator(back_points.begin()),
                           std::make_move_iterator(back_points.end()));

    // CW order of triangle indices
    std::vector<Vec3i> shape_triangles = triangulate(shape2d);
    result.indices.reserve(shape_triangles.size() * 2 + count_point * 2);
    // top triangles - change to CCW
    for (const Vec3i &t : shape_triangles)
        result.indices.emplace_back(t.x(), t.z(), t.y());
    // bottom triangles - use CW
    for (const Vec3i &t : shape_triangles)
        result.indices.emplace_back(t.x() + count_point, t.y() + count_point,
                                    t.z() + count_point);

    // quads around - zig zag by triangles
    size_t polygon_offset = 0;
    for (const Polygon &polygon : shape2d) {
        uint32_t polygon_points = polygon.points.size();
        for (uint32_t p = 0; p < polygon_points; p++) { 
            uint32_t i = polygon_offset + p;
            // previous index
            uint32_t ip = (p == 0) ? (polygon_offset + polygon_points - 1) : (i - 1);
            // bottom indices
            uint32_t i2  = i + count_point;
            uint32_t ip2 = ip + count_point;

            result.indices.emplace_back(i, i2, ip);
            result.indices.emplace_back(ip2, ip, i2);
        }
        polygon_offset += polygon_points;
    }
    return result;
}

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Triangulation_vertex_base_with_info_2.h>
Emboss::Indices Emboss::triangulate(const Points &   points,
                                       const HalfEdges &half_edges)
{
    // IMPROVE use int point insted of float !!!

    // use cgal triangulation
    using K    = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Itag = CGAL::Exact_predicates_tag;
    using CDT  = CGAL::Constrained_Delaunay_triangulation_2<K, CGAL::Default, Itag>;
    using Point = CDT::Point;

    // construct a constrained triangulation
    CDT                               cdt;
    std::map<CDT::Vertex_handle, uint32_t> map;             // for indices
    std::vector<CDT::Vertex_handle>   vertices_handle; // for constriants
    vertices_handle.reserve(points.size());
    for (const auto& p: points) {
        Point  cdt_p(p.x(), p.y());
        auto   handl = cdt.insert(cdt_p);
        vertices_handle.push_back(handl);
        // point index
        uint32_t pi = &p - &points.front();
        map[handl] = pi;
    }

    // triangle can not contain forbiden edge
    for (const std::pair<uint32_t, uint32_t> &edge : half_edges) {
        const CDT::Vertex_handle& vh1 = vertices_handle[edge.first];
        const CDT::Vertex_handle& vh2 = vertices_handle[edge.second];
        cdt.insert_constraint(vh1, vh2);
    }

    auto faces = cdt.finite_face_handles();
    std::vector<Vec3i> indices;
    indices.reserve(faces.size());
    for (CDT::Face_handle face : faces) {
        // point indices
        std::array<uint32_t, 3> pi;
        for (size_t i = 0; i < 3; ++i)
            pi[i] = map[face->vertex(i)];

        // Do not use triangles with opposit edges
        if (half_edges.find(std::make_pair(pi[0], pi[1])) != half_edges.end()) continue;
        if (half_edges.find(std::make_pair(pi[1], pi[2])) != half_edges.end()) continue;
        if (half_edges.find(std::make_pair(pi[2], pi[0])) != half_edges.end()) continue;

        indices.emplace_back(pi[0], pi[1], pi[2]);
    }    
    return indices;
}

Emboss::Indices Emboss::triangulate(const Polygon &polygon)
{
    const Points &                          pts = polygon.points;
    std::set<std::pair<uint32_t, uint32_t>> edges;
    for (uint32_t i = 1; i < pts.size(); ++i) edges.insert({i - 1, i});
    edges.insert({(uint32_t)pts.size() - 1, uint32_t(0)});
    Emboss::Indices indices = triangulate(pts, edges);
    remove_outer(indices, edges);
    return indices;
}

Emboss::Indices Emboss::triangulate(const Polygons &polygons)
{
    size_t count = count_points(polygons);
    Points points;
    points.reserve(count);
    for (const Polygon &polygon : polygons)
        points.insert(points.end(), polygon.points.begin(),
                      polygon.points.end());

    std::set<std::pair<uint32_t, uint32_t>> edges;
    uint32_t offset = 0;
    for (const Polygon& polygon : polygons) {
        const Points &pts = polygon.points;
        for (uint32_t i = 1; i < pts.size(); ++i) {
            uint32_t i2 = i + offset;
            edges.insert({i2 - 1, i2}); 
        }
        uint32_t size = static_cast<uint32_t>(pts.size());
        // add connection from first to last point
        edges.insert({offset + size - 1, offset});
        offset += size;
    }
    Emboss::Indices indices = triangulate(points, edges);
    remove_outer(indices, edges);
    return indices;
}

void Emboss::remove_outer(Indices &indices, const HalfEdges &half_edges) {
    uint32_t no_triangle = indices.size();
    std::map<HalfEdge, uint32_t> edge2triangle; 
    // triangles with all edges out of half_edge, candidate to remove
    std::vector<uint32_t> triangles_to_check;
    triangles_to_check.reserve(indices.size()/3);
    for (const auto& t : indices) { 
        uint32_t index = &t - &indices.front();
        bool     is_border = false;
        for (size_t j = 0; j < 3; ++j) { 
            size_t j2 = (j == 0) ? 2 : (j - 1);
            HalfEdge he(t[j], t[j2]);
            if (half_edges.find(he) != half_edges.end()) 
                is_border = true;            
            else
                edge2triangle[he] = index;
        }
        if (!is_border) { 
            triangles_to_check.push_back(index);
        }
    }

    std::set<uint32_t>   remove;
    std::queue<uint32_t> insert;
    for (uint32_t index : triangles_to_check) {
        auto it = remove.find(index);
        if (it != remove.end()) continue; // already removed
        
        bool is_edge = false;
        const Vec3i &t = indices[index];
        for (size_t j = 0; j < 3; ++j) {
            size_t j2 = (j == 0) ? 2 : (j - 1);
            // opposit 
            HalfEdge he(t[j2], t[j]);
            if (edge2triangle.find(he) == edge2triangle.end()) is_edge = true;
        }

        if (!is_edge) continue; // correct

        insert.push(index);
        while (!insert.empty()) {
            uint32_t i = insert.front();
            insert.pop();
            if (remove.find(i) != remove.end()) continue;
            remove.insert(i);

            for (size_t j = 0; j < 3; ++j) {
                size_t j2 = (j == 0) ? 2 : (j - 1);
                // opposit
                HalfEdge he(t[j2], t[j]);
                auto it = edge2triangle.find(he);
                if (it == edge2triangle.end()) continue; // edge
                insert.push(it->second);
            }
        }        
    }

    // remove indices
    std::vector<uint32_t> rem(remove.begin(), remove.end());
    std::sort(rem.begin(), rem.end());
    uint32_t offset = 0;
    for (uint32_t i : rem) {
        indices.erase(indices.begin() + (i - offset));
        ++offset;
    }
}

std::pair<Vec3f, Vec3f> Emboss::ProjectZ::project(const Point &p) const
{
    Vec3f front(p.x(),p.y(),0.f);
    Vec3f back = front; // copy
    back.z() = m_depth;
    return std::make_pair(front, back);
}
