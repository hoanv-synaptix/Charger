import re

def get_stage_xaml(title, unit_t, unit_c, t_prefix, c_prefix):
    return f"""
                                        <GroupBox Header="{title}" Background="#F8FAFC">
                                            <StackPanel>
                                                <StackPanel Orientation="Horizontal" Margin="0,0,0,10">
                                                    <CheckBox Content="Enabled" Margin="0,0,30,0" FontWeight="SemiBold"/>
                                                    <TextBlock Text="Delta ({unit_t}):" Style="{{StaticResource LabelStyle}}"/>
                                                    <TextBox Text="0" Width="80"/>
                                                </StackPanel>
                                                
                                                <Grid Margin="0,5">
                                                    <Grid.ColumnDefinitions>
                                                        <ColumnDefinition Width="100"/>
                                                        <ColumnDefinition Width="*"/>
                                                        <ColumnDefinition Width="*"/>
                                                        <ColumnDefinition Width="*"/>
                                                        <ColumnDefinition Width="*"/>
                                                        <ColumnDefinition Width="*"/>
                                                    </Grid.ColumnDefinitions>
                                                    <Grid.RowDefinitions>
                                                        <RowDefinition Height="Auto"/>
                                                        <RowDefinition Height="Auto"/>
                                                        <RowDefinition Height="Auto"/>
                                                        <RowDefinition Height="Auto"/>
                                                    </Grid.RowDefinitions>
                                                    
                                                    <!-- Thresholds Header -->
                                                    <TextBlock Grid.Row="0" Grid.Column="1" Text="{t_prefix} 1" Foreground="#3B82F6" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    <TextBlock Grid.Row="0" Grid.Column="2" Text="{t_prefix} 2" Foreground="#3B82F6" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    <TextBlock Grid.Row="0" Grid.Column="3" Text="{t_prefix} 3" Foreground="#3B82F6" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    <TextBlock Grid.Row="0" Grid.Column="4" Text="{t_prefix} 4" Foreground="#3B82F6" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    <TextBlock Grid.Row="0" Grid.Column="5" Text="{t_prefix} 5" Foreground="#3B82F6" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    
                                                    <!-- Thresholds Inputs -->
                                                    <TextBlock Grid.Row="1" Grid.Column="0" Text="Thresholds" Style="{{StaticResource LabelStyle}}"/>
                                                    <TextBox Grid.Row="1" Grid.Column="1" Text="0"/>
                                                    <TextBox Grid.Row="1" Grid.Column="2" Text="0"/>
                                                    <TextBox Grid.Row="1" Grid.Column="3" Text="0"/>
                                                    <TextBox Grid.Row="1" Grid.Column="4" Text="0"/>
                                                    <TextBox Grid.Row="1" Grid.Column="5" Text="0"/>

                                                    <!-- Currents Header -->
                                                    <TextBlock Grid.Row="2" Grid.Column="1" Text="{c_prefix} 1-2" Foreground="#8B5CF6" FontWeight="Bold" HorizontalAlignment="Center" Margin="0,10,0,0"/>
                                                    <TextBlock Grid.Row="2" Grid.Column="2" Text="{c_prefix} 2-3" Foreground="#8B5CF6" FontWeight="Bold" HorizontalAlignment="Center" Margin="0,10,0,0"/>
                                                    <TextBlock Grid.Row="2" Grid.Column="3" Text="{c_prefix} 3-4" Foreground="#8B5CF6" FontWeight="Bold" HorizontalAlignment="Center" Margin="0,10,0,0"/>
                                                    <TextBlock Grid.Row="2" Grid.Column="4" Text="{c_prefix} 4-5" Foreground="#8B5CF6" FontWeight="Bold" HorizontalAlignment="Center" Margin="0,10,0,0"/>
                                                    
                                                    <!-- Currents Inputs -->
                                                    <TextBlock Grid.Row="3" Grid.Column="0" Text="Current Limits" Style="{{StaticResource LabelStyle}}"/>
                                                    <TextBox Grid.Row="3" Grid.Column="1" Text="0"/>
                                                    <TextBox Grid.Row="3" Grid.Column="2" Text="0"/>
                                                    <TextBox Grid.Row="3" Grid.Column="3" Text="0"/>
                                                    <TextBox Grid.Row="3" Grid.Column="4" Text="0"/>
                                                </Grid>
                                            </StackPanel>
                                        </GroupBox>
"""

with open("MainWindow.xaml", "r", encoding="utf-8") as f:
    xaml = f.read()

# Replace the Charging Strategy GroupBox content
start_idx = xaml.find('<GroupBox Header="Charging Strategy"')
end_idx = xaml.find('</GroupBox>', start_idx) + 11

new_strategy = f"""<GroupBox Header="Charging Strategy" BorderBrush="#3B82F6" BorderThickness="2">
                                    <StackPanel>
{get_stage_xaml("Cell Voltage Stages", "V", "C", "Cell V", "Current")}
{get_stage_xaml("Temperature Stages", "C", "C", "Temp", "Current")}
{get_stage_xaml("SOC Stages", "%", "C", "SOC", "Current")}
                                    </StackPanel>
                                </GroupBox>"""

new_xaml = xaml[:start_idx] + new_strategy + xaml[end_idx:]

with open("MainWindow.xaml", "w", encoding="utf-8") as f:
    f.write(new_xaml)
