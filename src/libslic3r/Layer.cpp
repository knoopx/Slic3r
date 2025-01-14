#include "Layer.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Print.hpp"
#include "Fill/Fill.hpp"
#include "SVG.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

Layer::~Layer()
{
    this->lower_layer = this->upper_layer = nullptr;
    for (LayerRegion *region : m_regions)
        delete region;
    m_regions.clear();
}

// Test whether whether there are any slices assigned to this layer.
bool Layer::empty() const
{
    for (const LayerRegion *layerm : m_regions)
        if (layerm != nullptr && ! layerm->slices().empty())
            // Non empty layer.
            return false;
    return true;
}

LayerRegion* Layer::add_region(PrintRegion* print_region)
{
    m_regions.emplace_back(new LayerRegion(this, print_region));
    return m_regions.back();
}

// merge all regions' slices to get islands
void Layer::make_slices()
{
    ExPolygons slices;
    if (m_regions.size() == 1) {
        // optimization: if we only have one region, take its slices
        slices = m_regions.front()->slices();
    } else {
        Polygons slices_p;
        for (LayerRegion *layerm : m_regions)
            polygons_append(slices_p, to_polygons(layerm->slices()));
        slices = union_ex(slices_p);
    }
    
    this->slices.expolygons.clear();
    this->slices.expolygons.reserve(slices.size());
    
    // prepare ordering points
    Points ordering_points;
    ordering_points.reserve(slices.size());
    for (const ExPolygon &ex : slices)
        ordering_points.push_back(ex.contour.first_point());
    
    // sort slices
    std::vector<Points::size_type> order;
    Slic3r::Geometry::chained_path(ordering_points, order);
    
    // populate slices vector
    for (size_t i : order)
        this->slices.expolygons.push_back(std::move(slices[i]));
}

// Merge typed slices into untyped slices. This method is used to revert the effects of detect_surfaces_type() called for posPrepareInfill.
void Layer::merge_slices()
{
    if (m_regions.size() == 1) {
        // Optimization, also more robust. Don't merge classified pieces of layerm->slices,
        // but use the non-split islands of a layer. For a single region print, these shall be equal.
        m_regions.front()->m_slices.set(this->slices.expolygons, stPosInternal | stDensSparse);
    } else {
        for (LayerRegion *layerm : m_regions)
            // without safety offset, artifacts are generated (GH #2494)
            layerm->m_slices.set(union_ex(to_polygons(std::move(layerm->m_slices.surfaces)), true), stPosInternal | stDensSparse);
    }
}

ExPolygons Layer::merged(float offset_scaled) const
{
	assert(offset_scaled >= 0.f);
    // If no offset is set, apply EPSILON offset before union, and revert it afterwards.
	float offset_scaled2 = 0;
	if (offset_scaled == 0.f) {
		offset_scaled  = float(  EPSILON);
		offset_scaled2 = float(- EPSILON);
    }
    Polygons polygons;
    for (LayerRegion *layerm : m_regions)
		append(polygons, offset(to_expolygons(layerm->slices().surfaces), offset_scaled));
    ExPolygons out = union_ex(polygons);
	if (offset_scaled2 != 0.f)
		out = offset_ex(out, offset_scaled2);
    return out;
}

// Here the perimeters are created cummulatively for all layer regions sharing the same parameters influencing the perimeters.
// The perimeter paths and the thin fills (ExtrusionEntityCollection) are assigned to the first compatible layer region.
// The resulting fill surface is split back among the originating regions.
void Layer::make_perimeters()
{
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id();
    
    // keep track of regions whose perimeters we have already generated
    std::vector<unsigned char> done(m_regions.size(), false);
    
    for (LayerRegionPtrs::iterator layerm = m_regions.begin(); layerm != m_regions.end(); ++ layerm) {
        size_t region_id = layerm - m_regions.begin();
        if (done[region_id])
            continue;
        BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << ", region " << region_id;
        done[region_id] = true;
        const PrintRegionConfig &config = (*layerm)->region()->config();
        
        // find compatible regions
        LayerRegionPtrs layerms;
        layerms.push_back(*layerm);
        for (LayerRegionPtrs::const_iterator it = layerm + 1; it != m_regions.end(); ++it) {
            LayerRegion* other_layerm = *it;
            const PrintRegionConfig &other_config = other_layerm->region()->config();
            //FIXME get this list from the menu list itself to avoid duplication
            // also, this list is TOO SHORT
            /// !!! add here the settings you want to be added in the per-object menu.
            /// if you don't do that, objects will share the same region, and the same settings.
            if (config.perimeter_extruder           == other_config.perimeter_extruder
                && config.perimeters                == other_config.perimeters
                && config.only_one_perimeter_top    == other_config.only_one_perimeter_top
                && config.perimeter_speed           == other_config.perimeter_speed
                && config.external_perimeter_speed  == other_config.external_perimeter_speed
                && config.gap_fill_speed            == other_config.gap_fill_speed
                && config.overhangs                 == other_config.overhangs
                && config.overhangs_width           == other_config.overhangs_width
                && config.opt_serialize("perimeter_extrusion_width").compare(other_config.opt_serialize("perimeter_extrusion_width")) == 0
                && config.opt_serialize("external_perimeter_extrusion_width").compare(other_config.opt_serialize("external_perimeter_extrusion_width")) == 0
                && config.thin_walls                == other_config.thin_walls
                && config.thin_walls_min_width      == other_config.thin_walls_min_width
                && config.thin_walls_overlap        == other_config.thin_walls_overlap
                && config.external_perimeters_first == other_config.external_perimeters_first
                && config.infill_overlap            == other_config.infill_overlap
                && config.perimeter_loop            == other_config.perimeter_loop) {
                layerms.push_back(other_layerm);
                done[it - m_regions.begin()] = true;
            }
        }
        
        if (layerms.size() == 1) {  // optimization
            (*layerm)->fill_surfaces.surfaces.clear();
            (*layerm)->make_perimeters((*layerm)->slices(), &(*layerm)->fill_surfaces);
            (*layerm)->fill_expolygons = to_expolygons((*layerm)->fill_surfaces.surfaces);
        } else {
            SurfaceCollection new_slices;
            // Use the region with highest infill rate, as the make_perimeters() function below decides on the gap fill based on the infill existence.
            LayerRegion *layerm_config = layerms.front();
            {
                // group slices (surfaces) according to number of extra perimeters
                std::map<unsigned short, Surfaces> slices;  // extra_perimeters => [ surface, surface... ]
                for (LayerRegion *layerm : layerms) {
                    for (const Surface &surface : layerm->slices().surfaces)
                        slices[surface.extra_perimeters].emplace_back(surface);
                    if (layerm->region()->config().fill_density > layerm_config->region()->config().fill_density)
                    	layerm_config = layerm;
                }
                // merge the surfaces assigned to each group
                for (std::pair<const unsigned short,Surfaces> &surfaces_with_extra_perimeters : slices)
                    new_slices.append(union_ex(surfaces_with_extra_perimeters.second, true), surfaces_with_extra_perimeters.second.front());
            }
            
            // make perimeters
            SurfaceCollection fill_surfaces;
            layerm_config->make_perimeters(new_slices, &fill_surfaces);

            // assign fill_surfaces to each layer
            if (!fill_surfaces.surfaces.empty()) { 
                for (LayerRegionPtrs::iterator l = layerms.begin(); l != layerms.end(); ++l) {
                    // Separate the fill surfaces.
                    ExPolygons expp = intersection_ex(to_polygons(fill_surfaces), (*l)->slices());
                    (*l)->fill_expolygons = expp;
                    (*l)->fill_no_overlap_expolygons = (*layerm)->fill_no_overlap_expolygons;
                    (*l)->fill_surfaces.set(std::move(expp), fill_surfaces.surfaces.front());
                }
            }
        }
    }
    BOOST_LOG_TRIVIAL(trace) << "Generating perimeters for layer " << this->id() << " - Done";
}

void Layer::make_fills()
{
    #ifdef SLIC3R_DEBUG
    printf("Making fills for layer " PRINTF_ZU "\n", this->id());
    #endif
    for (LayerRegion *layerm : m_regions) {
        layerm->fills.clear();
        make_fill(*layerm, layerm->fills);
#ifndef NDEBUG
        for (size_t i = 0; i < layerm->fills.entities.size(); ++ i)
            assert(dynamic_cast<ExtrusionEntityCollection*>(layerm->fills.entities[i]) != NULL);
#endif
    }
}

void Layer::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const LayerRegion *region : m_regions)
        for (const Surface &surface : region->slices().surfaces)
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const LayerRegion *region : m_regions)
        for (const Surface &surface : region->slices().surfaces)
            svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close(); 
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void Layer::export_region_slices_to_svg_debug(const char *name) const
{
    static size_t idx = 0;
    this->export_region_slices_to_svg(debug_out_path("Layer-slices-%s-%d.svg", name, idx ++).c_str());
}

void Layer::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const LayerRegion *region : m_regions)
        for (const Surface &surface : region->slices().surfaces)
            bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const LayerRegion *region : m_regions)
        for (const Surface &surface : region->slices().surfaces)
            svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void Layer::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static size_t idx = 0;
    this->export_region_fill_surfaces_to_svg(debug_out_path("Layer-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

}
