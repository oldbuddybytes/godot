// AgentInspector: capture() and pick(), for looking at what the game actually rendered.

#include "agent_inspector.h"

#include "agent_common.h"

#include "core/config/engine.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "scene/gui/control.h"
#include "scene/gui/subviewport_container.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/world_2d.h"

#ifndef _3D_DISABLED
#include "scene/3d/camera_3d.h"
#include "scene/3d/physics/collision_object_3d.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#endif
#ifndef _2D_DISABLED
#include "scene/2d/camera_2d.h"
#include "scene/2d/physics/collision_object_2d.h"
#include "servers/physics_2d/physics_server_2d.h"
#endif

namespace {

String _node_path(const Node *p_node) {
	return p_node ? String(p_node->get_path()).trim_prefix("/root/") : String();
}

// The exact shape node under a collision, e.g. "Level/Door/Blocker", not just the body.
template <typename T>
String _shape_path(Object *p_collider, int p_shape) {
	T *collider = Object::cast_to<T>(p_collider);
	if (!collider) {
		return String();
	}
	Object *shape = collider->shape_owner_get_owner(collider->shape_find_owner(p_shape));
	return _node_path(Object::cast_to<Node>(shape ? shape : p_collider));
}

// The Viewport whose texture p_texture is, searching the tree under p_node.
Viewport *_viewport_for_texture(Node *p_node, const Ref<Texture2D> &p_texture) {
	Viewport *vp = Object::cast_to<Viewport>(p_node);
	if (vp && vp->get_texture() == p_texture) {
		return vp;
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (Viewport *found = _viewport_for_texture(p_node->get_child(i), p_texture)) {
			return found;
		}
	}
	return nullptr;
}

// The topmost visible TextureRect under p_position that shows a viewport's texture, e.g. a
// low-resolution 3D world scaled up to the window. Its mouse filter is often "ignore", so
// gui_find_control() does not see it. Does not descend into other viewports.
TextureRect *_find_screen_rect(Node *p_node, const Vector2 &p_position, Vector2 &r_local) {
	TextureRect *found = nullptr;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		Node *child = p_node->get_child(i);
		if (Object::cast_to<Viewport>(child)) {
			continue;
		}
		if (TextureRect *deeper = _find_screen_rect(child, p_position, r_local)) {
			found = deeper; // Later siblings draw on top.
		}
	}
	if (found) {
		return found;
	}
	TextureRect *rect = Object::cast_to<TextureRect>(p_node);
	if (rect && rect->is_visible_in_tree() && Object::cast_to<ViewportTexture>(rect->get_texture().ptr())) {
		const Vector2 local = rect->get_global_transform_with_canvas().affine_inverse().xform(p_position);
		if (Rect2(Vector2(), rect->get_size()).has_point(local)) {
			r_local = local;
			return rect;
		}
	}
	return nullptr;
}

// Picks inside the viewport shown by p_rect, mapping p_local (in the rect) to viewport pixels.
// Assumes the texture fills the rect (the default stretch modes).
void _pick_through_rect(TextureRect *p_rect, const Vector2 &p_local, Dictionary &r_result) {
	Viewport *inner = _viewport_for_texture(p_rect->get_tree()->get_root(), p_rect->get_texture());
	if (!inner) {
		// A ViewportTexture saved in a scene points to its viewport by path instead.
		const ViewportTexture *texture = Object::cast_to<ViewportTexture>(p_rect->get_texture().ptr());
		Node *scene = texture ? texture->get_local_scene() : nullptr;
		if (scene && !texture->get_viewport_path_in_scene().is_empty()) {
			inner = Object::cast_to<Viewport>(scene->get_node_or_null(texture->get_viewport_path_in_scene()));
		}
	}
	if (!inner || p_rect->get_size().x <= 0 || p_rect->get_size().y <= 0) {
		return;
	}
	const Vector2 inner_size = inner->get_visible_rect().size;
	r_result["inside"] = AgentInspector::pick(p_local / p_rect->get_size() * inner_size, inner);
}

} // namespace

Dictionary AgentInspector::capture(const String &p_path, double p_scale) {
	Dictionary result;
	SceneTree *tree = SceneTree::get_singleton();
	Viewport *viewport = tree ? tree->get_root() : nullptr;
	Ref<ViewportTexture> texture = viewport ? viewport->get_texture() : Ref<ViewportTexture>();
	Ref<Image> image = texture.is_valid() ? texture->get_image() : Ref<Image>();
	if (image.is_null() || image->is_empty()) {
		result["error"] = "No rendered frame. Rendering is disabled with --headless; use --offscreen.";
		return result;
	}
	if (p_scale > 0.0 && p_scale != 1.0) {
		image->resize(MAX(1, int(image->get_width() * p_scale)), MAX(1, int(image->get_height() * p_scale)), Image::INTERPOLATE_BILINEAR);
	}
	const String path = agent_output_path(p_path);
	DirAccess::make_dir_recursive_absolute(path.get_base_dir());
	if (image->save_png(path) != OK) {
		result["error"] = "Could not write " + path;
		return result;
	}

	result["path"] = path;
	result["frame"] = Engine::get_singleton()->get_frames_drawn();
	result["size"] = String(image->get_size());
	result["renderer"] = OS::get_singleton()->get_current_rendering_method();
	Dictionary camera;
#ifndef _3D_DISABLED
	if (Camera3D *cam = viewport->get_camera_3d()) {
		camera["node"] = _node_path(cam);
		camera["position"] = String(cam->get_global_position());
		camera["forward"] = String(-cam->get_global_basis().get_column(2).normalized());
		camera["fov"] = cam->get_fov();
		camera["near"] = cam->get_near();
		camera["far"] = cam->get_far();
	}
#endif
#ifndef _2D_DISABLED
	if (camera.is_empty()) {
		if (Camera2D *cam = viewport->get_camera_2d()) {
			camera["node"] = _node_path(cam);
			camera["position"] = String(cam->get_global_position());
			camera["zoom"] = String(cam->get_zoom());
		}
	}
#endif
	if (!camera.is_empty()) {
		result["camera"] = camera;
	}
	Ref<FileAccess> meta = FileAccess::open(path.get_basename() + ".json", FileAccess::WRITE);
	if (meta.is_valid()) {
		meta->store_string(JSON::stringify(result, "", false));
	}
	return result;
}

Dictionary AgentInspector::pick(const Vector2 &p_position, Object *p_viewport) {
	Dictionary result;
	Viewport *viewport = Object::cast_to<Viewport>(p_viewport);
	if (!viewport && SceneTree::get_singleton()) {
		viewport = SceneTree::get_singleton()->get_root();
	}
	if (!viewport) {
		result["error"] = "No viewport";
		return result;
	}
	result["viewport"] = _node_path(viewport);
	result["position"] = String(p_position);

	// A viewport shown by a TextureRect that ignores the mouse (the usual pixel-art setup).
	// It wins over a control that only contains it, such as the scene's root Control.
	Vector2 screen_local;
	TextureRect *screen = _find_screen_rect(viewport, p_position, screen_local);
	Control *control = viewport->gui_find_control(p_position);
	if (control && screen && control->is_ancestor_of(screen)) {
		control = nullptr;
	}

	// UI first, since it is drawn on top.
	if (control) {
		result["control"] = _node_path(control);
		// Follow input into a SubViewport, e.g. a terminal screen rendered from its own viewport.
		if (SubViewportContainer *container = Object::cast_to<SubViewportContainer>(control)) {
			for (int i = 0; i < container->get_child_count(); i++) {
				if (Viewport *sub = Object::cast_to<Viewport>(container->get_child(i))) {
					const Vector2 local = container->get_global_transform_with_canvas().affine_inverse().xform(p_position) / MAX(1, container->get_stretch_shrink());
					result["inside"] = pick(local, sub);
					break;
				}
			}
		}
		// Follow into a viewport shown by a TextureRect that receives mouse input.
		if (TextureRect *rect = Object::cast_to<TextureRect>(control)) {
			if (Object::cast_to<ViewportTexture>(rect->get_texture().ptr())) {
				_pick_through_rect(rect, rect->get_global_transform_with_canvas().affine_inverse().xform(p_position), result);
			}
		}
		return result;
	}

	if (screen) {
		result["screen"] = _node_path(screen);
		_pick_through_rect(screen, screen_local, result);
		return result;
	}

#ifndef _3D_DISABLED
	if (Camera3D *camera = viewport->get_camera_3d()) {
		PhysicsDirectSpaceState3D *space = camera->get_world_3d().is_valid() ? camera->get_world_3d()->get_direct_space_state() : nullptr;
		if (space) {
			PhysicsDirectSpaceState3D::RayParameters ray;
			ray.from = camera->project_ray_origin(p_position);
			ray.to = ray.from + camera->project_ray_normal(p_position) * camera->get_far();
			ray.collide_with_areas = true;
			PhysicsDirectSpaceState3D::RayResult hit;
			if (space->intersect_ray(ray, hit)) {
				result["object"] = _shape_path<CollisionObject3D>(hit.collider, hit.shape);
				result["hit_position"] = String(hit.position);
				result["distance"] = ray.from.distance_to(hit.position);
				return result;
			}
		}
	}
#endif
#ifndef _2D_DISABLED
	Ref<World2D> world = viewport->find_world_2d();
	if (world.is_valid() && world->get_direct_space_state()) {
		PhysicsDirectSpaceState2D::PointParameters point;
		point.position = viewport->get_canvas_transform().affine_inverse().xform(p_position);
		point.collide_with_areas = true;
		PhysicsDirectSpaceState2D::ShapeResult hits[1];
		if (world->get_direct_space_state()->intersect_point(point, hits, 1) > 0) {
			result["object"] = _shape_path<CollisionObject2D>(hits[0].collider, hits[0].shape);
			result["hit_position"] = String(point.position);
			return result;
		}
	}
#endif
	result["object"] = Variant(); // Nothing under this pixel.
	return result;
}
