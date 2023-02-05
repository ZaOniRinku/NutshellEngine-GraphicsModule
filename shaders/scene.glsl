// Scene
Object ground(vec3 p) {
	float dist = 0.0;
	dist -= 2.0 * noise(p.xz * 0.002);
	dist += 32.0 * noise(p.xz * 0.02) * noise(p.xz * 0.008) + 8.0;
	dist += 2.0 * fbm(p.xz * 0.05, 2);
	dist += 20.0 * fbm(p.xz * 0.02, 2) * noise(p.xz * 0.02);
	dist += p.y - 22.0;

	float colorFactor = p.y / 220.0;
	return Object(dist, Material(vec3(0.1, 0.57, 0.03) * colorFactor, vec2(0.0, fbm(p.xz * 0.02, 4))));
}

Object water(vec3 p) {
	float dist = shPlane(p, vec3(0.0, 1.0, 0.0), 10.0 + cos(pC.time) * 3.0);

	return Object(dist, Material(vec3(0.09, 0.56, 0.64), vec2(0.5, 1.0)));
}

Object node(vec3 p) {
	float time = pC.time / 100.0;
	float a = sin(2.0 * time);
	float dist = shSphere(p - vec3(0.0, 20.0, 0.0), 300.0);

	return Object(dist, Material(vec3(1.0, 0.0, 0.0), vec2(0.0, 0.0)));
}

Object root(vec3 p) {
	float time = pC.time / 100.0;
	float wiggleTime = 3.0;
	float wiggleIntensity = 50.0;
	vec3 center = vec3(-pC.rootCenterPosition.x, pC.rootCenterPosition.y + cos(pC.time * wiggleTime) * wiggleIntensity + 50.0, pC.rootCenterPosition.z);
	vec3 head = vec3(-pC.rootHeadPosition.x, pC.rootHeadPosition.y + sin(pC.time * wiggleTime) * wiggleIntensity + 50.0, pC.rootHeadPosition.z);
	vec3 tail = vec3(-pC.rootTailPosition.x, pC.rootTailPosition.y - 30.0, pC.rootTailPosition.z);
	float dist = shBezier(p, head, center, tail);

	return Object(dist - 15.0, Material(vec3(0.33, 0.66, 0.9), vec2(0.0, 1.0)));
}

Object scene(vec3 p) {
	Object ground = ground(p);
	Object water = water(p);
	Object node = node(p);
	Object root = root(p);

	Object scene = opSmoothUnion(ground, water, 2.0);
	scene = opSmoothUnion(scene, opOnion(node, 40.0), 1.0);
	scene = opSmoothUnion(scene, root, 3.0);

	for (int i = 0; i < light.nbLights; i++) {
		if (light.lights[i].position.w == 0.0) {
			vec3 lightPosition = vec3(-light.lights[i].position.x, light.lights[i].position.yz);
			float dist = shSphere(p - lightPosition, 100.0);
			Object lightObject = Object(dist, Material(vec3(1.0, 1.0, 1.0), vec2(1.0, 0.0)));
			scene = opUnion(scene, lightObject);
		}
	}

	return scene;
}

// Background
vec3 background(vec3 p) {
	return vec3(0.1, 0.1, 0.1);
}

// Camera
vec3 up = vec3(0.0, 1.0, 0.0);

mat3 camera() {
	const vec3 forward = normalize(vec3(-pC.cameraDirection.x, pC.cameraDirection.yz));
	const vec3 right = normalize(cross(up, forward));
	const vec3 realUp = cross(forward, right);

	return mat3(right, -realUp, forward);
}