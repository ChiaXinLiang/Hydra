"""Entry points for hydra."""
from hydra_python._plugins import habitat
import hydra_python as hydra
from spark_dsg.open3d_visualization import DsgVisualizer, RemoteVisualizer
import subprocess
import click
import pathlib
import zmq
import os


class DsgPublisher:
    """Quick zmq publisher for graph."""

    def __init__(self, url="tcp://127.0.0.1:8001", num_per_update=5):
        """Make a publisher."""
        self._context = zmq.Context()
        self._socket = self._context.socket(zmq.PUB)
        self._socket.bind(url)
        self._num_update_calls = 0
        self._num_calls_per_update = num_per_update

    def update_graph(self, G, force=False):
        """Publish graph."""
        should_update = self._num_update_calls & self._num_calls_per_update == 0
        if should_update or force:
            self._socket.send(G.to_binary(include_mesh=True))

        self._num_update_calls += 1

    def stop(self):
        """Stop the publisher."""
        pass


def _get_trajectory(data, prev_dsg, seed, use_full_scene=False):
    if prev_dsg:
        prev_path = pathlib.Path(prev_dsg).resolve()
        poses = hydra.Trajectory.from_scene_graph(str(prev_path))
    elif use_full_scene:
        poses = data.get_full_trajectory(inflation_radius=0.25, seed=seed)
        if poses is None:
            click.secho(
                "Failed to find trajectory for single room! Defaulting to random",
                fg="yellow",
            )
            poses = data.get_random_trajectory(inflation_radius=0.25, seed=seed)
    else:
        poses = data.get_random_trajectory(inflation_radius=0.25, seed=seed)

    click.secho(f"Trajectory is {poses.get_path_length()} meters long", fg="green")
    return poses


@click.group()
def cli():
    """Entry point target for subcommands."""
    pass


@cli.command(name="run")
@click.argument("scene_path", type=click.Path(exists=True))
@click.option("-o", "--output-path", default=None)
@click.option("-l", "--label-space", default="ade20k_mp3d")
@click.option("-p", "--prev-dsg", default=None, help="dsg containing trajectory")
@click.option("-s", "--seed", default=None, help="random seed")
@click.option("-v", "--visualize", default=False, help="start visualizer", is_flag=True)
@click.option("-g", "--glog-level", default=0, help="minimum glog level")
@click.option("-f", "--use-full-scene", is_flag=True, help="use-full-scene")
@click.option("-y", "--force", is_flag=True, help="overwrite previous output")
@click.option("-p", "--publish", is_flag=True, help="publish graph to zmq")
@click.option("--verbosity", default=0, help="glog verbosity")
@click.option("--show-images", default=False, help="show semantics", is_flag=True)
@click.option("--show-config", default=False, help="show hydra config", is_flag=True)
@click.option("--show-progress", default=False, help="show progress bar", is_flag=True)
@click.option("--config-verbosity", default=0, help="glog verbosity to print configs")
def run(
    scene_path,
    output_path,
    label_space,
    prev_dsg,
    seed,
    visualize,
    glog_level,
    use_full_scene,
    force,
    publish,
    verbosity,
    show_images,
    show_config,
    show_progress,
    config_verbosity,
):
    """Run Hydra against mp3d."""
    hydra.set_glog_level(glog_level, verbosity)
    output_path = hydra.resolve_output_path(output_path, force=force)
    data = habitat.HabitatInterface(scene_path)
    poses = _get_trajectory(data, prev_dsg, seed, use_full_scene=use_full_scene)

    if publish and visualize:
        click.secho("Cannot publish and visualize graph.")
        return False

    configs = hydra.load_configs(
        "habitat", data.camera_info, labelspace_name=label_space
    )
    if not configs:
        click.secho(
            f"Invalid config: dataset 'habitat' and label space '{label_space}'",
            fg="red",
        )
        return

    pipeline_config = hydra.PipelineConfig(configs)
    pipeline_config.enable_reconstruction = True
    pipeline_config.label_names = {i: x for i, x in enumerate(data.colormap.names)}
    data.colormap.fill_label_space(pipeline_config.label_space)
    if output_path:
        pipeline_config.logs.log_dir = str(output_path)

    pipeline = hydra.HydraPipeline(
        pipeline_config, robot_id=0, config_verbosity=config_verbosity
    )
    pipeline.init(configs)
    if output_path:
        glog_dir = output_path / "logs"
        if not glog_dir.exists():
            glog_dir.mkdir()
        # TODO(nathan) make pathlib bindings
        hydra.set_glog_dir(str(glog_dir))

    if visualize:
        visualizer = DsgVisualizer(start_remote=False)
        visualizer.update_graph(pipeline.graph)
    elif publish:
        visualizer = DsgPublisher()
    else:
        visualizer = None

    try:
        hydra.run(
            pipeline,
            data,
            poses,
            visualizer=visualizer,
            show_images=show_images,
            show_progress=show_progress,
        )
    finally:
        pipeline.save()
        if visualizer is not None:
            visualizer.stop()


@cli.command(name="reconstruction")
@click.argument("scene_path", type=click.Path(exists=True))
@click.option("-o", "--output-path", default=None)
@click.option("-l", "--label-space", default="ade20k_mp3d")
@click.option("-p", "--prev-dsg", default=None, help="dsg containing trajectory")
@click.option("-s", "--seed", default=None, help="random seed")
@click.option("-g", "--glog-level", default=0, help="minimum glog level")
@click.option("-f", "--use-full-scene", is_flag=True, help="use-full-scene")
@click.option("-y", "--force", is_flag=True, help="overwrite previous output")
@click.option("--verbosity", default=0, help="glog verbosity")
@click.option("--show-images", default=False, help="show semantics", is_flag=True)
@click.option("--show-progress", default=False, help="show progress bar", is_flag=True)
@click.option("--voxel-size", default=0.1, type=float)
def reconstruction(
    scene_path,
    output_path,
    label_space,
    prev_dsg,
    seed,
    glog_level,
    use_full_scene,
    force,
    verbosity,
    show_images,
    show_progress,
    voxel_size,
):
    """Run Hydra against mp3d."""
    hydra.set_glog_level(glog_level, verbosity)
    output_path = hydra.resolve_output_path(output_path, force=force)
    data = habitat.HabitatInterface(scene_path)
    poses = _get_trajectory(data, prev_dsg, seed, use_full_scene=use_full_scene)

    configs = hydra.load_reconstruction_configs(
        "habitat",
        data.camera_info,
        labelspace_name=label_space,
        voxel_size=voxel_size,
    )
    if not configs:
        click.secho(
            f"Invalid config: dataset 'habitat' and label space '{label_space}'",
            fg="red",
        )
        return

    pipeline_config = hydra.PipelineConfig(configs)
    pipeline_config.label_names = {i: x for i, x in enumerate(data.colormap.names)}
    data.colormap.fill_label_space(pipeline_config.label_space)
    if output_path:
        pipeline_config.logs.log_dir = str(output_path)

    pipeline = hydra.HydraReconstruction(pipeline_config, configs)

    try:
        hydra.run(
            pipeline,
            data,
            poses,
            show_images=show_images,
            show_progress=show_progress,
            step_callback=None,
        )
    finally:
        pipeline.save()


@cli.command(name="visualize")
def visualize():
    """Run Hydra visualizer."""
    visualizer = RemoteVisualizer(num_dynamic_to_skip=1)
    visualizer.run()


@cli.command(name="mp3d")
@click.argument("mp3d_path", type=click.Path(exists=True))
@click.argument("output")
@click.option("-m", "--model_file", type=click.Path(exists=True), default=None)
@click.option("-s", "--seed", default=None, help="random seed")
@click.option("-v", "--visualize", default=False, help="start visualizer", is_flag=True)
@click.option("-f", "--use-full-scene", is_flag=True, help="use-full-scene")
def run_mp3d(mp3d_path, output, model_file, seed, visualize, use_full_scene):
    """Run Hydra visualizer."""
    mp3d_path = pathlib.Path(mp3d_path).expanduser().absolute()
    output = pathlib.Path(output).expanduser().absolute()
    mp3d_scenes = [x for x in mp3d_path.glob("**/*.glb")]
    for scene in mp3d_scenes:
        output_path = output / scene.stem
        args = ["hydra", "run", str(scene), "-o", str(output_path)]
        if model_file:
            args += ["--model_file", str(model_file)]

        if seed:
            args += ["--seed", str(seed)]

        if visualize:
            args += ["--publish"]

        if use_full_scene:
            args += ["--use-full-scene"]

        args += ["--show-progress", "--force", "--config-verbosity", "1"]

        env = os.environ.copy()
        env["GLOG_minloglevel"] = "2"
        subprocess.run(args, env=env)


@cli.command(name="mp3d-reconstruction")
@click.argument("mp3d_path", type=click.Path(exists=True))
@click.argument("output")
@click.option("--show-images", default=False, help="show semantics", is_flag=True)
@click.option("--voxel-size", default=0.1, type=float)
def run_mp3d_reconstruction(mp3d_path, output, show_images, voxel_size):
    """Run Hydra visualizer."""
    mp3d_path = pathlib.Path(mp3d_path).expanduser().absolute()
    output = pathlib.Path(output).expanduser().absolute()
    mp3d_scenes = [x for x in mp3d_path.glob("**/*.glb")]
    for scene in mp3d_scenes:
        output_path = output / scene.stem
        args = ["hydra", "reconstruction", str(scene), "-o", str(output_path)]
        args += ["--show-progress", "--force", "--use-full-scene"]
        args += ["--voxel-size", str(voxel_size)]

        if show_images:
            args += ["--show_images"]

        env = os.environ.copy()
        env["GLOG_minloglevel"] = "2"
        subprocess.run(args, env=env)


@cli.command(name="mp3d-graph")
@click.argument("input_paths", type=click.Path(exists=True), nargs=-1)
@click.option("-v", "--verbosity", default=0)
def run_mp3d_graph(input_paths, verbosity):
    """Make a scene graph from a volumetric map."""
    hydra.set_glog_level(0, verbosity)
    configs = hydra.load_configs("habitat", {}, labelspace_name="ade20k_mp3d")
    if not configs:
        click.secho(
            "Invalid config: dataset 'habitat' and label space 'ade20k_mp3d'",
            fg="red",
        )
        return

    pipeline_config = hydra.PipelineConfig(configs)
    # TODO(nathan) colormap
    graph_builder = hydra.BatchPipeline(pipeline_config)

    click.echo(f"processing {len(input_paths)} graph(s):")
    for input_path in input_paths:
        input_path = pathlib.Path(input_path).expanduser().absolute()
        click.echo(f"  - {input_path} ... ", nl=False)
        map_path = input_path / "map"
        dense_map = hydra.VolumetricMap.load(map_path)

        graph = graph_builder.construct(configs, dense_map)
        graph.save(str(input_path / "dsg_with_mesh.json"), include_mesh=True)
        graph.save(str(input_path / "dsg.json"), include_mesh=False)
        click.secho("done", fg="green")