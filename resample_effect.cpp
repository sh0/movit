// Three-lobed Lanczos, the most common choice.
#define LANCZOS_RADIUS 3.0

#include <math.h>
#include <assert.h>

#include "resample_effect.h"
#include "effect_chain.h"
#include "util.h"
#include "opengl.h"

namespace {

float sinc(float x)
{
	if (fabs(x) < 1e-6) {
		return 1.0f - fabs(x);
	} else {
		return sin(x) / x;
	}
}

float lanczos_weight(float x, float a)
{
	if (fabs(x) > a) {
		return 0.0f;
	} else {
		return sinc(M_PI * x) * sinc(M_PI * x / a);
	}
}

}  // namespace

ResampleEffect::ResampleEffect()
	: input_width(1280),
	  input_height(720)
{
	register_int("width", &output_width);
	register_int("height", &output_height);

	// The first blur pass will forward resolution information to us.
	hpass = new SingleResamplePassEffect(this);
	hpass->set_int("direction", SingleResamplePassEffect::HORIZONTAL);
	vpass = new SingleResamplePassEffect(NULL);
	vpass->set_int("direction", SingleResamplePassEffect::VERTICAL);

	update_size();
}

void ResampleEffect::rewrite_graph(EffectChain *graph, Node *self)
{
	Node *hpass_node = graph->add_node(hpass);
	Node *vpass_node = graph->add_node(vpass);
	graph->connect_nodes(hpass_node, vpass_node);
	graph->replace_receiver(self, hpass_node);
	graph->replace_sender(self, vpass_node);
	self->disabled = true;
} 

// We get this information forwarded from the first blur pass,
// since we are not part of the chain ourselves.
void ResampleEffect::inform_input_size(unsigned input_num, unsigned width, unsigned height)
{
	assert(input_num == 0);
	assert(width != 0);
	assert(height != 0);
	input_width = width;
	input_height = height;
	update_size();
}
		
void ResampleEffect::update_size()
{
	bool ok = true;
	ok |= hpass->set_int("input_width", input_width);
	ok |= hpass->set_int("input_height", input_height);
	ok |= hpass->set_int("output_width", output_width);
	ok |= hpass->set_int("output_height", input_height);

	ok |= vpass->set_int("input_width", output_width);
	ok |= vpass->set_int("input_height", input_height);
	ok |= vpass->set_int("output_width", output_width);
	ok |= vpass->set_int("output_height", output_height);

	assert(ok);
}

bool ResampleEffect::set_float(const std::string &key, float value) {
	if (key == "width") {
		output_width = value;
		update_size();
		return true;
	}
	if (key == "height") {
		output_height = value;
		update_size();
		return true;
	}
	return false;
}

SingleResamplePassEffect::SingleResamplePassEffect(ResampleEffect *parent)
	: parent(parent),
	  direction(HORIZONTAL),
 	  input_width(1280),
 	  input_height(720),
	  last_input_width(-1),
	  last_input_height(-1),
	  last_output_width(-1),
	  last_output_height(-1)
{
	register_int("direction", (int *)&direction);
	register_int("input_width", &input_width);
	register_int("input_height", &input_height);
	register_int("output_width", &output_width);
	register_int("output_height", &output_height);

	glGenTextures(1, &texnum);
}

SingleResamplePassEffect::~SingleResamplePassEffect()
{
	glDeleteTextures(1, &texnum);
}

std::string SingleResamplePassEffect::output_fragment_shader()
{
	char buf[256];
	sprintf(buf, "#define DIRECTION_VERTICAL %d\n", (direction == VERTICAL));
	return buf + read_file("resample_effect.frag");
}

// Using vertical scaling as an example:
//
// Generally out[y] = w0 * in[yi] + w1 * in[yi + 1] + w2 * in[yi + 2] + ...
//
// Obviously, yi will depend on y (in a not-quite-linear way), but so will
// the weights w0, w1, w2, etc.. The easiest way of doing this is to encode,
// for each sample, the weight and the yi value, e.g. <yi, w0>, <yi + 1, w1>,
// and so on. For each y, we encode these along the x-axis (since that is spare),
// so out[0] will read from parameters <x,y> = <0,0>, <1,0>, <2,0> and so on.
//
// For horizontal scaling, we fill in the exact same texture;
// the shader just interprets is differently.
//
// TODO: Support optimization of wrapping the sample texture.
// TODO: Support optimization using free linear sampling, like in BlurEffect.
void SingleResamplePassEffect::update_texture(GLuint glsl_program_num, const std::string &prefix, unsigned *sampler_num)
{
	unsigned src_size, dst_size;
	if (direction == SingleResamplePassEffect::HORIZONTAL) {
		assert(input_height == output_height);
		src_size = input_width;
		dst_size = output_width;
	} else if (direction == SingleResamplePassEffect::VERTICAL) {
		assert(input_width == output_width);
		src_size = input_height;
		dst_size = output_height;
	} else {
		assert(false);
	}

	// Sample the kernel in the right place. A diagram with a triangular kernel
	// (corresponding to linear filtering, and obviously with radius 1)
	// for easier ASCII art drawing:
	//
	//                *
	//               / \                      |
	//              /   \                     |
	//             /     \                    |
	//    x---x---x   x   x---x---x---x
	//
	// Scaling up (in this case, 2x) means sampling more densely:
	//
	//                *
	//               / \                      |
	//              /   \                     |
	//             /     \                    |
	//   x-x-x-x-x-x x x x-x-x-x-x-x-x-x
	//
	// When scaling up, any destination pixel will only be influenced by a few
	// (in this case, two) neighboring pixels, and more importantly, the number
	// will not be influenced by the scaling factor. (Note, however, that the
	// pixel centers have moved, due to OpenGL's center-pixel convention.)
	// The only thing that changes is the weights themselves, as the sampling
	// points are at different distances from the original pixels.
	//
	// Scaling down is a different story:
	//
	//                *
	//               / \                      |
	//              /   \                     |
	//             /     \                    |
	//    --x------ x     --x-------x--
	//
	// Again, the pixel centers have moved in a maybe unintuitive fashion,
	// although when you consider that there are multiple source pixels around,
	// it's not so bad as at first look:
	//
	//            *   *   *   *
	//           / \ / \ / \ / \              |
	//          /   X   X   X   \             |
	//         /   / \ / \ / \   \            |
	//    --x-------x-------x-------x--
	//
	// As you can see, the new pixels become averages of the two neighboring old
	// ones (the situation for Lanczos is of course more complex).
	//
	// Anyhow, in this case we clearly need to look at more source pixels
	// to compute the destination pixel, and how many depend on the scaling factor.
	// Thus, the kernel width will vary with how much we scale.
	float radius_scaling_factor = std::min(float(dst_size) / float(src_size), 1.0f);
	int int_radius = lrintf(LANCZOS_RADIUS / radius_scaling_factor);
	src_samples = int_radius * 2 + 1;
	float *weights = new float[dst_size * src_samples * 2];
	for (unsigned y = 0; y < dst_size; ++y) {
		// Find the point around which we want to sample the source image,
		// compensating for differing pixel centers as the scale changes.
		float center_src_y = (y + 0.5f) * float(src_size) / float(dst_size) - 0.5f;
		int base_src_y = lrintf(center_src_y);

		// Now sample <int_radius> pixels on each side around that point.
		for (int i = 0; i < src_samples; ++i) {
			int src_y = base_src_y + i - int_radius;
			float weight = lanczos_weight(radius_scaling_factor * (src_y - center_src_y), LANCZOS_RADIUS);
			weights[(y * src_samples + i) * 2 + 0] = weight * radius_scaling_factor;
			weights[(y * src_samples + i) * 2 + 1] = (src_y + 0.5) / float(src_size);
		}
	}

	// Encode as a two-component texture. Note the GL_REPEAT, which is not relevant
	// right now, but will be later.
	glActiveTexture(GL_TEXTURE0 + *sampler_num);
	check_error();
	glBindTexture(GL_TEXTURE_2D, texnum);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	check_error();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, src_samples, dst_size, 0, GL_RG, GL_FLOAT, weights);
	check_error();

	delete[] weights;

}

void SingleResamplePassEffect::set_gl_state(GLuint glsl_program_num, const std::string &prefix, unsigned *sampler_num)
{
	Effect::set_gl_state(glsl_program_num, prefix, sampler_num);

	if (input_width != last_input_width ||
	    input_height != last_input_height ||
	    output_width != last_output_width ||
	    output_height != last_output_height) {
		update_texture(glsl_program_num, prefix, sampler_num);
		last_input_width = input_width;
		last_input_height = input_height;
		last_output_width = output_width;
		last_output_height = output_height;
	}

	glActiveTexture(GL_TEXTURE0 + *sampler_num);
	check_error();
	glBindTexture(GL_TEXTURE_2D, texnum);
	check_error();

	set_uniform_int(glsl_program_num, prefix, "sample_tex", *sampler_num);
	++sampler_num;
	set_uniform_int(glsl_program_num, prefix, "num_samples", src_samples);

	// Instructions for how to convert integer sample numbers to positions in the weight texture.
	set_uniform_float(glsl_program_num, prefix, "sample_x_scale", 1.0f / src_samples);
	set_uniform_float(glsl_program_num, prefix, "sample_x_offset", 0.5f / src_samples);

	// We specifically do not want mipmaps on the input texture;
	// they break minification.
	glActiveTexture(GL_TEXTURE0);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
}